#include "postgres.h"
#include "access/htup_details.h"
#include "executor/tuptable.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "access/heapam.h" /* For ExecCopySlotHeapTuple */
#include "utils/builtins.h" /* For pg_detoast_datum */


#include "optimized_row_format.h"
#include "orf_slot.h"
#include "orf_utils.h"

/*
 * Custom logging for optimized row format extension
 * DISABLED for testing - uncomment to enable debugging
 */
// #define OPTIMIZED_LOG(fmt, ...) \
//     elog(NOTICE, "OPTIMIZED_DEBUG: " fmt, ##__VA_ARGS__)
#define OPTIMIZED_LOG(fmt, ...) do { } while (0)

/*
 * optimized_slot_init_cache
 *      Initialize cached data pointers in the slot to reduce memory indirection.
 *      This function calculates commonly accessed pointers once and stores them
 *      in the slot structure for fast access during attribute extraction.
 */
void
optimized_slot_init_cache(OptimizedTupleTableSlot *opt_slot)
{
	HeapTuple tuple = opt_slot->opt_tuple;
	char *data_start;
	uint32 *var_col_count_ptr;
	
	if (!tuple || !tuple->t_data)
	{
		opt_slot->cache_valid = false;
		return;
	}
	
	/* Cache the tuple header */
	opt_slot->cached_header = (OptimizedTupleHeader) tuple->t_data;
	
	/* Calculate and cache commonly accessed data pointers */
	data_start = (char *) opt_slot->cached_header + opt_slot->cached_header->t_hoff;
	
	if (HeapTupleHasNulls(tuple))
	{
		/* Skip null bitmap if present */
		int null_bitmap_len = BITMAPLEN(opt_slot->base.tts_tupleDescriptor->natts);
		data_start += MAXALIGN(null_bitmap_len);
	}
	
	/* Cache variable column count and offsets array pointer */
	var_col_count_ptr = (uint32 *) MAXALIGN(data_start);
	opt_slot->var_col_count = *var_col_count_ptr;
	opt_slot->var_offsets = (uint32 *) ((char *) var_col_count_ptr + MAXALIGN(sizeof(uint32)));
	
	/* Cache fixed data start pointer */
	opt_slot->fixed_data_start = (char *)opt_slot->var_offsets + MAXALIGN(opt_slot->var_col_count * sizeof(uint32));
	
	opt_slot->cache_valid = true;
	
	OPTIMIZED_LOG("optimized_slot_init_cache: cached header=%p, fixed_data_start=%p, var_offsets=%p, var_col_count=%u",
		opt_slot->cached_header, opt_slot->fixed_data_start, opt_slot->var_offsets, opt_slot->var_col_count);
}

/*
 * optimized_getattr_direct
 *      Fast path for single attribute access using cached slot data.
 *      This function minimizes overhead by using pre-computed pointers
 *      and specialized handling for common data types.
 */
Datum
optimized_getattr_direct(OptimizedTupleTableSlot *opt_slot, int attnum, bool *isnull)
{
	Form_pg_attribute att = TupleDescAttr(opt_slot->base.tts_tupleDescriptor, attnum - 1);
	OptimizedColumnMapCache *cache = opt_slot->column_cache;
	
	/* Ensure cache is initialized */
	if (!opt_slot->cache_valid)
	{
		optimized_slot_init_cache(opt_slot);
		if (!opt_slot->cache_valid)
		{
			/* Fallback to regular extraction */
			return optimized_extract_attribute(opt_slot->opt_tuple, attnum, 
											 opt_slot->base.tts_tupleDescriptor, cache, isnull);
		}
	}
	
	/* Fast validation with cached data */
	if (!cache || attnum < 1 || attnum > cache->natts)
	{
		*isnull = true;
		return (Datum) 0;
	}

	*isnull = false;
	
	/* Fast path for fixed-length columns (most common case) */
	if (att->attlen > 0)
	{
		uint32 fixed_off = cache->fixed_offsets[attnum - 1];
		char *data_ptr = opt_slot->fixed_data_start + fixed_off;
		
		/* Optimized extraction for common integer types */
		if (att->attbyval)
		{
			switch (att->attlen)
			{
				case sizeof(int32):
					return Int32GetDatum(*((int32 *) data_ptr));
				case sizeof(int16):
					return Int16GetDatum(*((int16 *) data_ptr));
				case sizeof(char):
					return CharGetDatum(*((char *) data_ptr));
#if SIZEOF_DATUM == 8
				case sizeof(Datum):
					return *((Datum *) data_ptr);
#endif
				default:
					break; /* Fall through to regular handling */
			}
		}
		/* For non-byval fixed types, return pointer */
		return PointerGetDatum(data_ptr);
	}
	else /* Variable-length column */
	{
		int target_var_index = cache->var_indexes[attnum - 1];
		
		if (target_var_index < opt_slot->var_col_count)
		{
			uint32 absolute_offset = opt_slot->var_offsets[target_var_index];
			char *var_data_ptr = (char *)opt_slot->cached_header + absolute_offset;
			return PointerGetDatum(var_data_ptr);
		}
		else
		{
			*isnull = true;
			return (Datum) NULL;
		}
	}
}

/*
 * Custom getsomeattrs function for on-demand attribute fetching.
 * This is called when the executor needs specific attributes from the slot.
 */
void
optimized_getsomeattrs(TupleTableSlot *slot, int natts)
{
	OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
	int attnum;

	OPTIMIZED_LOG("optimized_getsomeattrs: called with natts=%d, current tts_nvalid=%d", 
	              natts, slot->tts_nvalid);

	/*
	 * ADVANCED OPTIMIZATION: Detect single-column projection pattern
	 * Use fast path for single attribute access to minimize overhead
	 */
	if (natts == 1 && slot->tts_nvalid == 0)
	{
		/* Single-column projection - use fast path */
		slot->tts_values[0] = optimized_getattr_direct(opt_slot, 1, &slot->tts_isnull[0]);
		slot->tts_nvalid = 1;
		
		/* Mark as cached if we use attr_cached tracking */
		if (opt_slot->attr_cached)
			opt_slot->attr_cached[0] = true;
		
		OPTIMIZED_LOG("optimized_getsomeattrs: used fast path for single column");
		return;
	}

	/*
	 * PROJECTION OPTIMIZATION: Only extract attributes that haven't been
	 * extracted yet and are within the requested range.
	 * This is the key to making SELECT col1 FROM table fast!
	 */
	
	/* Ensure we have the attribute cache initialized */
	if (!opt_slot->attr_cached)
	{
		opt_slot->attr_cached = (bool *) palloc0(
			slot->tts_tupleDescriptor->natts * sizeof(bool));
	}

	/* Only extract attributes that we haven't cached yet */
	for (attnum = slot->tts_nvalid + 1; attnum <= natts; attnum++)
	{
		if (!opt_slot->attr_cached[attnum - 1])
		{
			/* Extract this specific attribute on-demand */
			slot->tts_values[attnum - 1] = optimized_getattr_for_slot(slot, attnum, &slot->tts_isnull[attnum - 1]);
			opt_slot->attr_cached[attnum - 1] = true;
		}
	}

	/* Update the number of valid attributes */
	slot->tts_nvalid = natts;
}

Datum
optimized_getattr_for_slot(TupleTableSlot *slot, int attnum, bool *isnull)
{
	OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
	HeapTuple tuple = opt_slot->opt_tuple;
	TupleDesc tupleDesc = slot->tts_tupleDescriptor;
	OptimizedColumnMapCache *cache = opt_slot->column_cache;

	/*
	 * PERFORMANCE OPTIMIZATION: Direct extraction with minimal overhead
	 * Use our custom optimized_extract_attribute function with O(1) cache lookup
	 * to read data from the optimized tuple format efficiently.
	 */
	return optimized_extract_attribute(tuple, attnum, tupleDesc, cache, isnull);
}


/*
 * Custom slot operations for optimized tables
 * These implement minimal required functionality while delegating to heap operations where possible
 */
static void optimized_slot_init(TupleTableSlot *slot)
{
	/* Initialize our custom optimized slot */
	OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
	opt_slot->opt_tuple = NULL;
	opt_slot->attr_cached = NULL;
}

static void optimized_slot_release(TupleTableSlot *slot)
{
	/* Clean up our custom slot resources */
	OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
	
	if (opt_slot->attr_cached)
	{
		pfree(opt_slot->attr_cached);
		opt_slot->attr_cached = NULL;
	}
}

static void optimized_slot_clear(TupleTableSlot *slot)
{
	OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;

	if (TTS_SHOULDFREE(slot))
	{
		if (opt_slot->opt_tuple)
		{
			heap_freetuple(opt_slot->opt_tuple);
			opt_slot->opt_tuple = NULL;
		}
		slot->tts_flags &= ~TTS_FLAG_SHOULDFREE;
	}

	slot->tts_nvalid = 0;
	slot->tts_flags |= TTS_FLAG_EMPTY;
	ItemPointerSetInvalid(&slot->tts_tid);
	
	/* Reset attribute cache tracking */
	if (opt_slot->attr_cached)
	{
		memset(opt_slot->attr_cached, false, 
			slot->tts_tupleDescriptor->natts * sizeof(bool));
	}
}

static void optimized_slot_materialize(TupleTableSlot *slot)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;
	MemoryContext oldContext;
	TupleDesc tupdesc;
	int natts;
	Datum *values;
	bool *isnull;
	int i;
	HeapTuple heap_tuple;

	Assert(!TTS_EMPTY(slot));
	
	if (TTS_SHOULDFREE(slot))
		return;
	
	oldContext = MemoryContextSwitchTo(slot->tts_mcxt);
	
	slot->tts_nvalid = 0;
	hslot->off = 0;
	
	if (!hslot->tuple)
	{
		/* No tuple yet, create from values/isnull arrays */
		hslot->tuple = heap_form_tuple(slot->tts_tupleDescriptor,
									   slot->tts_values,
									   slot->tts_isnull);
	}
	else
	{
		/*
		 * CRITICAL FIX: We have an optimized format tuple, but materialization
		 * means we need a standard heap tuple (e.g., for ORDER BY operations).
		 * Convert optimized format to standard heap format.
		 */
		tupdesc = slot->tts_tupleDescriptor;
		natts = tupdesc->natts;
		values = (Datum *) palloc0(natts * sizeof(Datum));
		isnull = (bool *) palloc0(natts * sizeof(bool));
		
		/* Extract each attribute from optimized format */
		for (i = 0; i < natts; i++)
		{
			values[i] = optimized_extract_attribute_no_cache(hslot->tuple, i + 1, tupdesc, &isnull[i]);
			
			/* Handle detoasting for variable-length attributes */
			if (!isnull[i] && tupdesc->attrs[i].attlen == -1)
			{
				values[i] = PointerGetDatum(pg_detoast_datum((struct varlena *) DatumGetPointer(values[i])));
			}
		}
		
		/* Create a new standard heap tuple from the extracted values */
		heap_tuple = heap_form_tuple(tupdesc, values, isnull);
		
		/* Copy tuple metadata */
		heap_tuple->t_self = hslot->tuple->t_self;
		heap_tuple->t_tableOid = hslot->tuple->t_tableOid;
		
		/* Replace the optimized tuple with the standard heap tuple */
		hslot->tuple = heap_tuple;
		
		/* Clean up */
		pfree(values);
		pfree(isnull);
	}
	
	slot->tts_flags |= TTS_FLAG_SHOULDFREE;
	
	MemoryContextSwitchTo(oldContext);
}

static void optimized_slot_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
	HeapTuple	tuple;
	MemoryContext oldcontext;
	HeapTupleTableSlot *hslot;

	oldcontext = MemoryContextSwitchTo(dstslot->tts_mcxt);
	tuple = ExecCopySlotHeapTuple(srcslot);
	MemoryContextSwitchTo(oldcontext);

	/*
	 * Manually store the tuple to preserve custom slot operations.
	 * Using ExecStoreHeapTuple would fail with "trying to store a heap tuple 
	 * into wrong type of slot" because it checks TTS_IS_HEAPTUPLE.
	 */
	hslot = (HeapTupleTableSlot *) dstslot;
	
	/* Clear the destination slot first */
	ExecClearTuple(dstslot);
	
	/* Manually store the tuple data while preserving custom operations */
	dstslot->tts_nvalid = 0;
	hslot->tuple = tuple;
	hslot->off = 0;
	dstslot->tts_flags &= ~(TTS_FLAG_EMPTY | TTS_FLAG_SHOULDFREE);
	dstslot->tts_tid = tuple->t_self;
	dstslot->tts_tableOid = tuple->t_tableOid;
	
	/* Set TTS_FLAG_SHOULDFREE since we own the copied tuple */
	dstslot->tts_flags |= TTS_FLAG_SHOULDFREE;
}

static HeapTuple optimized_slot_get_heap_tuple(TupleTableSlot *slot)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;
	bool is_optimized_table;
	TupleDesc tupdesc;
	int natts;
	Datum *values;
	bool *isnull;
	int i;
	HeapTuple heap_tuple;

	Assert(!TTS_EMPTY(slot));
	if (!hslot->tuple)
		optimized_slot_materialize(slot);
	
	/*
	 * CRITICAL FIX: ORDER BY operations call this function expecting standard heap format.
	 * We need to convert optimized format tuples to standard heap format, but only
	 * when the tuple actually comes from our optimized table (not from INSERT operations).
	 * 
	 * Detection strategy: Check if the tuple's table uses optimized_row_format AM.
	 */
	
	/* Check if this tuple came from an optimized table */
	is_optimized_table = false;
	if (hslot->tuple && hslot->tuple->t_tableOid != InvalidOid)
	{
		Relation rel = try_relation_open(hslot->tuple->t_tableOid, NoLock);
		if (rel != NULL)
		{
			/* Check if the table uses a non-heap access method (i.e., our optimized AM) */
			if (rel->rd_tableam != GetHeapamTableAmRoutine())
			{
				is_optimized_table = true;
			}
			relation_close(rel, NoLock);
		}
	}
	
	if (is_optimized_table)
	{
		/* This tuple is from an optimized table - convert to standard heap format */
		tupdesc = slot->tts_tupleDescriptor;
		natts = tupdesc->natts;
		values = (Datum *) palloc0(natts * sizeof(Datum));
		isnull = (bool *) palloc0(natts * sizeof(bool));
		
		/* Extract each attribute from optimized format */
		for (i = 0; i < natts; i++)
		{
			values[i] = optimized_extract_attribute_no_cache(hslot->tuple, i + 1, tupdesc, &isnull[i]);
			
			/* Handle detoasting for variable-length attributes */
			if (!isnull[i] && tupdesc->attrs[i].attlen == -1)
			{
				values[i] = PointerGetDatum(pg_detoast_datum((struct varlena *) DatumGetPointer(values[i])));
			}
		}
		
		/* Create a new standard heap tuple from the extracted values */
		heap_tuple = heap_form_tuple(tupdesc, values, isnull);
		
		/* Copy tuple metadata */
		heap_tuple->t_self = hslot->tuple->t_self;
		heap_tuple->t_tableOid = hslot->tuple->t_tableOid;
		
		/* Clean up */
		pfree(values);
		pfree(isnull);
		
		return heap_tuple;
	}
	
	/* This tuple is already in standard heap format */
	return hslot->tuple;
}

static HeapTuple optimized_slot_copy_heap_tuple(TupleTableSlot *slot)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;
	bool is_optimized_table;
	HeapTuple converted;

	Assert(!TTS_EMPTY(slot));
	if (!hslot->tuple)
		optimized_slot_materialize(slot);
	
	/*
	 * Same logic as get_heap_tuple: check if tuple is from optimized table.
	 */
	is_optimized_table = false;
	if (hslot->tuple && hslot->tuple->t_tableOid != InvalidOid)
	{
		Relation rel = try_relation_open(hslot->tuple->t_tableOid, NoLock);
		if (rel != NULL)
		{
			/* Non-heap AM implies our optimized AM for this table */
			if (rel->rd_tableam != GetHeapamTableAmRoutine())
			{
				is_optimized_table = true;
			}
			relation_close(rel, NoLock);
		}
	}
	
	if (is_optimized_table)
	{
		/* Convert optimized format tuple to standard heap format, then copy */
		converted = optimized_slot_get_heap_tuple(slot);
		return heap_copytuple(converted);
	}
	
	return heap_copytuple(hslot->tuple);
}

static MinimalTuple optimized_slot_copy_minimal_tuple(TupleTableSlot *slot)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;
	MemoryContext oldcontext;
	TupleDesc tupdesc;
	int natts;
	Datum *values;
	bool *isnull;
	int i;
	MinimalTuple mtup;

	Assert(!TTS_EMPTY(slot));
	
	oldcontext = MemoryContextSwitchTo(slot->tts_mcxt);
	
	tupdesc = slot->tts_tupleDescriptor;
	natts = tupdesc->natts;
	values = (Datum *) palloc0(natts * sizeof(Datum));
	isnull = (bool *) palloc0(natts * sizeof(bool));
	
	/* Extract each attribute from optimized format */
	for (i = 0; i < natts; i++)
	{
		values[i] = optimized_extract_attribute_no_cache(hslot->tuple, i + 1, tupdesc, &isnull[i]);
		
		/* Handle detoasting for variable-length attributes */
		if (!isnull[i] && tupdesc->attrs[i].attlen == -1)
		{
			values[i] = PointerGetDatum(pg_detoast_datum((struct varlena *) DatumGetPointer(values[i])));
		}
	}
	
	/* Form a proper MinimalTuple in the slot memory context */
	mtup = heap_form_minimal_tuple(tupdesc, values, isnull);
	
	pfree(values);
	pfree(isnull);
	MemoryContextSwitchTo(oldcontext);
	
	return mtup;
}

static Datum optimized_slot_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;
	
	Assert(!TTS_EMPTY(slot));
	
	if (!hslot->tuple)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot retrieve a system column in this context")));
	
	return heap_getsysattr(hslot->tuple, attnum, slot->tts_tupleDescriptor, isnull);
}

/*
 * Custom TupleTableSlotOps for optimized row format.
 * Based on TTSOpsHeapTuple but overrides getsomeattrs to use our optimized attribute extraction.
 */
static const TupleTableSlotOps TTSOpsOptimized = {
	.base_slot_size = sizeof(OptimizedTupleTableSlot),
	.init = optimized_slot_init,
	.release = optimized_slot_release,
	.clear = optimized_slot_clear,
	.getsomeattrs = optimized_getsomeattrs,		/* Use our custom function */
	.getsysattr = optimized_slot_getsysattr,	/* Use our wrapper function */
	.is_current_xact_tuple = slot_is_current_xact_tuple,	/* Use exported function */
	.materialize = optimized_slot_materialize,
	.copyslot = optimized_slot_copyslot,
	.get_heap_tuple = optimized_slot_get_heap_tuple,

	/* A heap tuple table slot can not "own" a minimal tuple. */
	.get_minimal_tuple = NULL,
	.copy_heap_tuple = optimized_slot_copy_heap_tuple,
	.copy_minimal_tuple = optimized_slot_copy_minimal_tuple
};

/*
 * optimized_slot_callbacks - Return the appropriate TupleTableSlotOps for optimized tables
 *
 * This is called by the PostgreSQL executor to determine what type of slot
 * operations to use for our table AM. This is critical for projection optimization.
 * 
 * CRITICAL FIX: For UPDATE operations, we need to return buffer tuple slot operations
 * to satisfy the TTS_IS_BUFFERTUPLE assertion in heapam_handler.c
 */
const TupleTableSlotOps *
optimized_slot_callbacks(Relation relation)
{
    // For now, return heap buffer tuple operations to fix UPDATE crashes
    // TODO: Implement proper optimized slot operations that are compatible with UPDATE
    return &TTSOpsBufferHeapTuple; 
}
