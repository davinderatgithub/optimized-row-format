#include "postgres.h"

/* Required for most Postgres development */
#include "access/heapam.h"
#include "access/hio.h"
#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/tupdesc.h"
#include "access/tupmacs.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "optimized_row_format.h"

PG_MODULE_MAGIC;

/*
 * Custom logging for optimized row format extension
 * This allows us to control logging independently of PostgreSQL's general debug level
 * ENABLED for UPDATE operation debugging
 */
#define OPTIMIZED_LOG(fmt, ...) \
    elog(NOTICE, "OPTIMIZED_DEBUG: " fmt, ##__VA_ARGS__)

/*
 * Optimized Storage Format
 * ------------------------
 * This module implements an optimized storage format for PostgreSQL tables that
 * reorganizes columns to improve access patterns and reduce overhead.
 *
 * Memory Layout:
 * -------------
 * The tuple is organized in the following order:
 *
 * 1. Tuple Header (SizeofOptimizedTupleHeader bytes)
 *    - Contains standard PostgreSQL tuple header fields
 *    - t_hoff points to the start of the null bitmap (if present) or variable column count
 *
 * 2. Null Bitmap (BITMAPLEN(natts) bytes) - CONDITIONAL
 *    - One bit per column indicating NULL status
 *    - 1 = not null, 0 = null
 *    - Only present when HEAP_HASNULL is set (i.e., when there are actual null values)
 *    - IMPORTANT: The null bitmap maintains the user's original column order
 *      rather than the physical storage order. This means:
 *      - Bit 0 corresponds to the first column in the user's table definition
 *      - Bit 1 corresponds to the second column, and so on
 *      - This is different from the physical layout where fixed-length columns
 *        come first, followed by variable-length columns
 *    - Example: For a table with columns (a int, b text, c int, d text):
 *      - Original order: [a, b, c, d]
 *      - Physical order: [a, c, b, d] (fixed-length first)
 *      - Null bitmap order: [a, b, c, d] (matches original)
 *    - Note: This design allows direct copying of null bits between standard
 *      and optimized formats, maintaining compatibility with PostgreSQL's
 *      tuple access functions and catalog information.
 *
 * 3. Variable-Length Offsets Array (var_col_count * sizeof(uint32) bytes)
 *    - Array of absolute offsets pointing to variable-length column data
 *    - Each offset is relative to the start of the tuple header (not relative to variable data section)
 *    - This eliminates the need to compute fixed data length during extraction
 *    - Only present if table has variable-length columns
 *
 * 4. Fixed-Length Columns (fixed_data_len bytes)
 *    - All fixed-length columns stored contiguously
 *    - Columns are stored in their original order
 *    - No padding between columns
 *
 * 5. Variable-Length Data (var_data_len bytes)
 *    - All variable-length columns stored contiguously
 *    - Each column's data is stored in full (including varlena header)
 *    - Absolute offsets array provides direct access to each column's data
 *
 * Alignment:
 * ---------
 * - Header is MAXALIGN'd
 * - Null bitmap is MAXALIGN'd (when present)
 * - Variable offsets array is MAXALIGN'd
 * - Fixed-length data section is MAXALIGN'd
 * - Variable-length data section is MAXALIGN'd
 *
 * Benefits:
 * --------
 * 1. Efficient access to fixed-length columns (no offset calculations needed)
 * 2. Quick NULL checks using bitmap (when present)
 * 3. Fast access to variable-length columns through absolute offset array
 * 4. Better cache utilization due to data locality
 * 5. Reduced overhead compared to standard heap format
 * 6. Space efficiency: no null bitmap when no null values exist
 * 7. No need to compute fixed data length during extraction (performance improvement)
 *
 * Null Bitmap Access:
 * -----------------
 * When accessing null values in a tuple:
 * 1. Check if HEAP_HASNULL is set in t_infomask
 * 2. If set, use the original column number (attnum) to check the null bitmap
 * 3. If not set, all columns are non-null (no bitmap present)
 * 4. The data is stored in the order: fixed-length columns first, then variable-length columns
 * 5. Column positions are calculated dynamically based on the table schema
 *
 * Example:
 * - Table: CREATE TABLE t (a int, b text, c int, d text);
 * - Insert: INSERT INTO t VALUES (1, NULL, 2, 'hello');
 * - Null bitmap: [1,0,1,1] (1=not null, 0=null) - present because of NULL
 * - Physical layout: [1,2,NULL,'hello']
 * - Variable offsets: [offset_to_b, offset_to_d] (absolute from tuple start)
 *
 * - Insert: INSERT INTO t VALUES (1, 'hello', 2, 'world');
 * - No null bitmap (all values non-null)
 * - Physical layout: [1,2,'hello','world']
 * - Variable offsets: [offset_to_b, offset_to_d] (absolute from tuple start)
 */


/* Forward declarations */
static TableAmRoutine optimized_tableam;

/* --- Custom TupleTableSlotOps for on-demand attribute fetching --- */
static Datum optimized_getattr_for_slot(TupleTableSlot *slot, int attnum, bool *isnull);

/*
 * Based on TTSOpsHeapTuple from src/backend/executor/execTuples.c
 * We override only the getattr function to use our custom logic.
 */
/*
 * Custom slot operations for optimized row format.
 * We'll use the heap tuple slot but override getsomeattrs for projection optimization.
 */
static void optimized_getsomeattrs(TupleTableSlot *slot, int natts);

/* We'll create a simpler approach by overriding the slot operations after ExecStoreHeapTuple */


/* --- Custom scan descriptor that embeds TableScanDescData --- */
typedef struct OptimizedScanDescData
{
    TableScanDescData rs_base;  /* AM independent part of the descriptor */
	HeapTupleData o_ctup;       /* Current tuple in scan */

    /* Private scan state for optimized row format */
    Relation rel;
    BlockNumber nblocks;
    BlockNumber currBlock;
    OffsetNumber currOffset;
    Buffer currBuffer;
    OptimizedColumnMapCache *column_cache;  /* Cache for O(1) attribute access */
} OptimizedScanDescData;

typedef OptimizedScanDescData *OptimizedScanDesc;

/* Custom tuple table slot for optimized row format with projection optimization */
typedef struct OptimizedTupleTableSlot
{
	TupleTableSlot base;		/* Base slot structure */
	HeapTuple opt_tuple;		/* The optimized tuple data */
	bool *attr_cached;			/* Track which attributes have been extracted */
	OptimizedColumnMapCache *column_cache;  /* Cache for O(1) attribute access */
	
	/* PERFORMANCE OPTIMIZATION: Cached data to reduce memory indirection */
	OptimizedTupleHeader cached_header;  /* Cached tuple header */
	char *fixed_data_start;      		/* Cached pointer to fixed data section */
	uint32 *var_offsets;         		/* Cached pointer to variable offsets array */
	uint32 var_col_count;        		/* Cached variable column count */
	bool cache_valid;            		/* Whether cached pointers are valid */
} OptimizedTupleTableSlot;

/* Get the heap AM routine to delegate operations to */
static const TableAmRoutine *
get_heap_am_routine(void)
{
    return GetHeapamTableAmRoutine();
}

/* Function declarations */
static void optimized_slot_init_cache(OptimizedTupleTableSlot *opt_slot);
static OffsetEncodingType choose_offset_encoding(Size estimated_tuple_size, int var_col_count);
static TM_Result optimized_tuple_delete(Relation relation, ItemPointer tid,
                                       CommandId cid, Snapshot crosscheck, Snapshot snapshot,
                                       bool wait, TM_FailureData *tmfd, bool changingPart);
static TM_Result optimized_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
                                       CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait,
                                       TM_FailureData *tmfd, LockTupleMode *lockmode,
                                       TU_UpdateIndexes *update_indexes);
static TableScanDesc optimized_scan_begin(Relation rel, Snapshot snapshot,
                                        int nkeys, struct ScanKeyData *key,
                                        ParallelTableScanDesc pscan, uint32 flags);
static void optimized_scan_end(TableScanDesc scan);
static void optimized_scan_rescan(TableScanDesc scan, ScanKey key, bool set_params,
                                 bool allow_strat, bool allow_sync, bool allow_pagemode);
static bool optimized_scan_getnextslot(TableScanDesc scan, ScanDirection direction,
                                     TupleTableSlot *slot);
static Datum optimized_getattr(HeapTuple tuple, int attnum,
                             TupleDesc tupleDesc, bool *isnull);
static Datum optimized_extract_attribute(HeapTuple tuple, int attnum, TupleDesc tupleDesc, 
                                        OptimizedColumnMapCache *cache, bool *isnull);
static void optimized_tuple_insert(Relation relation, TupleTableSlot *slot,
                                 CommandId cid, int options, struct BulkInsertStateData *bistate);
static void __attribute__((unused))
optimized_tuple_insert_speculative(Relation relation, TupleTableSlot *slot,
                                 CommandId cid, int options, struct BulkInsertStateData *bistate,
                                 uint32 specToken);
static void __attribute__((unused))
optimized_tuple_complete_speculative(Relation relation, TupleTableSlot *slot,
                                   uint32 specToken, bool succeeded);
static Size optimized_parallelscan_estimate(Relation rel);
static bool optimized_relation_needs_toast_table(Relation rel);
static const TupleTableSlotOps *optimized_slot_callbacks(Relation relation);
static OptimizedColumnMapCache *build_column_cache(TupleDesc tupleDesc);

/* Index Scan Functions */
static IndexFetchTableData *optimized_index_fetch_begin(Relation rel);
static void optimized_index_fetch_reset(IndexFetchTableData *scan);
static void optimized_index_fetch_end(IndexFetchTableData *scan);
static bool optimized_index_fetch_tuple(struct IndexFetchTableData *scan,
                                      ItemPointer tid,
                                      Snapshot snapshot,
                                      TupleTableSlot *slot,
                                      bool *call_again, bool *all_dead);

/* Table access method handler */
static bool optimized_tableam_initialized = false;

/* Table AM handler function */
PG_FUNCTION_INFO_V1(optimized_row_format_tableam_handler);
Datum
optimized_row_format_tableam_handler(PG_FUNCTION_ARGS)
{
	if (!optimized_tableam_initialized)
	{
		const TableAmRoutine *heap_am = get_heap_am_routine();

		/*
		 * Copy the entire heap AM routine and then override the functions
		 * we want to implement. This is more robust than initializing
		 * everything manually.
		 */
		memcpy(&optimized_tableam, heap_am, sizeof(TableAmRoutine));

		/* Override with our custom functions */
		optimized_tableam.type = T_TableAmRoutine;
		optimized_tableam.scan_begin = optimized_scan_begin;
		optimized_tableam.scan_end = optimized_scan_end;
		optimized_tableam.scan_rescan = optimized_scan_rescan;
		optimized_tableam.scan_getnextslot = optimized_scan_getnextslot;
		optimized_tableam.parallelscan_estimate = optimized_parallelscan_estimate;
		                            optimized_tableam.tuple_insert = optimized_tuple_insert;
            optimized_tableam.tuple_delete = optimized_tuple_delete;
            optimized_tableam.tuple_update = optimized_tuple_update;
            optimized_tableam.relation_needs_toast_table = optimized_relation_needs_toast_table;
            optimized_tableam.slot_callbacks = optimized_slot_callbacks;

		/* Index scan functions */
		optimized_tableam.index_fetch_begin = optimized_index_fetch_begin;
		optimized_tableam.index_fetch_reset = optimized_index_fetch_reset;
		optimized_tableam.index_fetch_end = optimized_index_fetch_end;
		optimized_tableam.index_fetch_tuple = optimized_index_fetch_tuple;

		optimized_tableam_initialized = true;
	}

    PG_RETURN_POINTER(&optimized_tableam);
}

/* --- Update scan_begin to use the new embedded structure --- */
static TableScanDesc
optimized_scan_begin(Relation rel, Snapshot snapshot,
                    int nkeys, struct ScanKeyData *key,
                    ParallelTableScanDesc pscan, uint32 flags)
{
    OptimizedScanDesc oscan = (OptimizedScanDesc) palloc0(sizeof(OptimizedScanDescData));
    TableScanDesc scan = &oscan->rs_base;
    TupleDesc tupleDesc = RelationGetDescr(rel);

    /* Initialize the base scan descriptor */
    scan->rs_rd = rel;
    scan->rs_snapshot = snapshot;
    scan->rs_nkeys = nkeys;
    scan->rs_key = key;
    scan->rs_parallel = NULL;
    scan->rs_flags = flags;

    /* Initialize our private fields */
    oscan->rel = rel;
    oscan->nblocks = RelationGetNumberOfBlocks(rel);
    oscan->currBlock = 0;
    oscan->currOffset = FirstOffsetNumber;
    oscan->currBuffer = InvalidBuffer;

    /* Build and cache column mapping information for O(1) attribute access */
    if (rel->rd_amcache == NULL)
    {
        rel->rd_amcache = build_column_cache(tupleDesc);
        OPTIMIZED_LOG("optimized_scan_begin: built new cache for relation %s",
                     RelationGetRelationName(rel));
    }
    oscan->column_cache = (OptimizedColumnMapCache *) rel->rd_amcache;

    return scan;
}

/* --- Update scan_end to use the new embedded structure --- */
static void
optimized_scan_end(TableScanDesc scan)
{
    OptimizedScanDesc oscan = (OptimizedScanDesc) scan;

    if (BufferIsValid(oscan->currBuffer))
        ReleaseBuffer(oscan->currBuffer);
    pfree(oscan);
}

/* --- Update scan_rescan to use the new embedded structure --- */
static void
optimized_scan_rescan(TableScanDesc scan, ScanKey key, bool set_params,
                     bool allow_strat, bool allow_sync, bool allow_pagemode)
{
    OptimizedScanDesc oscan = (OptimizedScanDesc) scan;

    /* Update scan keys */
    scan->rs_nkeys = key ? scan->rs_nkeys : 0;
    scan->rs_key = key;

    /* Reset scan state */
    oscan->currBlock = 0;
    oscan->currOffset = FirstOffsetNumber;
    if (BufferIsValid(oscan->currBuffer))
    {
        ReleaseBuffer(oscan->currBuffer);
        oscan->currBuffer = InvalidBuffer;
    }
}

/* --- Implement minimal direct scan using the new embedded structure --- */
/*
 * optimized_scan_getnextslot
 * 
 * This implementation follows the recommended approach for projection optimization:
 * - It does NOT deform/extract all attributes up front.
 * - It stores the raw tuple in the slot and sets up the slot for on-demand attribute extraction.
 * - The slot's tts_ops should be set to the custom TupleTableSlotOps elsewhere (not here).
 */
static bool
optimized_scan_getnextslot(TableScanDesc scan, ScanDirection direction,
                          TupleTableSlot *slot)
{
    OptimizedScanDesc oscan = (OptimizedScanDesc) scan;
    HeapTuple tuple = &oscan->o_ctup;
    Page page;
    OffsetNumber maxOffset;
    Snapshot snapshot = scan->rs_snapshot;

    while (oscan->currBlock < oscan->nblocks)
    {
        /* Read the current block if not already loaded */
        if (!BufferIsValid(oscan->currBuffer))
            oscan->currBuffer = ReadBuffer(oscan->rel, oscan->currBlock);
        LockBuffer(oscan->currBuffer, BUFFER_LOCK_SHARE);
        page = BufferGetPage(oscan->currBuffer);
        maxOffset = PageGetMaxOffsetNumber(page);

        while (oscan->currOffset <= maxOffset)
        {
            ItemId itemId = PageGetItemId(page, oscan->currOffset);
            if (ItemIdIsUsed(itemId))
            {
                /* Build HeapTupleData pointing to the tuple */
                tuple->t_len = ItemIdGetLength(itemId);
                tuple->t_data = (HeapTupleHeader) PageGetItem(page, itemId);
                tuple->t_tableOid = RelationGetRelid(oscan->rel);
                ItemPointerSet(&tuple->t_self, oscan->currBlock, oscan->currOffset);

                if (HeapTupleSatisfiesVisibility(tuple, snapshot, oscan->currBuffer))
                {
                    /*
                    * SLOT-BASED PROJECTION OPTIMIZATION:
                    * Store the raw optimized tuple directly in our custom slot.
                    * No attribute extraction happens here - it's all on-demand!
                    * 
                    * When the executor needs specific attributes, it will call our
                    * custom getsomeattrs function which will extract ONLY the
                    * requested attributes from the optimized format.
                    */
                    
                    // Cast to our custom slot type
                    OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
                    
                    ExecClearTuple(slot);
                    
                    // Store the raw optimized tuple (NO EXTRACTION YET!)
                    opt_slot->opt_tuple = heap_copytuple(tuple);
                    
                    // Pass the column cache to the slot for O(1) attribute access
                    opt_slot->column_cache = oscan->column_cache;
                    
                    // PERFORMANCE OPTIMIZATION: Initialize cached data pointers
                    optimized_slot_init_cache(opt_slot);
                    
                    // Initialize attribute cache tracking
                    if (!opt_slot->attr_cached) {
                        opt_slot->attr_cached = (bool *) palloc0(
                            slot->tts_tupleDescriptor->natts * sizeof(bool));
                    } else {
                        memset(opt_slot->attr_cached, false, 
                            slot->tts_tupleDescriptor->natts * sizeof(bool));
                    }
                    
                    // Mark slot as having data but NO extracted attributes yet
                    slot->tts_flags &= ~TTS_FLAG_EMPTY;
                    slot->tts_flags |= TTS_FLAG_SHOULDFREE;
                    slot->tts_nvalid = 0;  // CRITICAL: No attributes extracted yet!
                    slot->tts_tid = tuple->t_self;
                    slot->tts_tableOid = RelationGetRelid(oscan->rel);

                    /* Advance offset for next call */
                    oscan->currOffset++;
                    LockBuffer(oscan->currBuffer, BUFFER_LOCK_UNLOCK);
                    return true;
                }
            }
            oscan->currOffset++;
        }
        /* Done with this block */
        LockBuffer(oscan->currBuffer, BUFFER_LOCK_UNLOCK);
        ReleaseBuffer(oscan->currBuffer);
        oscan->currBuffer = InvalidBuffer;
        oscan->currBlock++;
        oscan->currOffset = FirstOffsetNumber;
    }
    return false;
}

static Size
optimized_parallelscan_estimate(Relation rel)
{
	/* For now, delegate to heap AM since scan is not fully implemented */
	const TableAmRoutine *heap_am = get_heap_am_routine();
	return heap_am->parallelscan_estimate(rel);
}

/*
 * build_column_cache
 *      Build a cache of column position mappings to eliminate O(N) lookups.
 *      This pre-computes which columns are fixed vs variable-length and their
 *      positions in the optimized tuple format.
 */
static OptimizedColumnMapCache *
build_column_cache(TupleDesc tupleDesc)
{
	OptimizedColumnMapCache *cache;
	int i, var_col_index = 0;
	Size current_fixed_offset = 0;
	MemoryContext oldcontext;
	
	OPTIMIZED_LOG("build_column_cache: Building cache for %d attributes", tupleDesc->natts);
	
	/* 
	 * CRITICAL FIX: Allocate cache in CacheMemoryContext to prevent it from being
	 * freed when the current memory context is reset. The cache needs to persist
	 * for the lifetime of the relation as it's stored in rel->rd_amcache.
	 */
	oldcontext = MemoryContextSwitchTo(CacheMemoryContext);
	
	cache = (OptimizedColumnMapCache *) palloc0(sizeof(OptimizedColumnMapCache));
	cache->natts = tupleDesc->natts;
	
	/* Allocate arrays for mappings */
	cache->fixed_offsets = (uint32 *) palloc0(tupleDesc->natts * sizeof(uint32));
	cache->var_indexes = (int *) palloc0(tupleDesc->natts * sizeof(int));
	
	MemoryContextSwitchTo(oldcontext);
	
	/* Analyze each column and build the cache */
	for (i = 0; i < tupleDesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupleDesc, i);
		
		if (att->attisdropped)
		{
			/* Dropped columns: mark as invalid */
			cache->fixed_offsets[i] = UINT32_MAX;
			cache->var_indexes[i] = -1;
			continue;
		}
			
		if (att->attlen > 0) /* Fixed-length column */
		{
			cache->fixed_offsets[i] = current_fixed_offset;
			cache->var_indexes[i] = -1; /* Not a variable column */
			current_fixed_offset += att->attlen;
			
			OPTIMIZED_LOG("build_column_cache: Column %d (%s) fixed-length, offset=%u", 
						  i + 1, NameStr(att->attname), cache->fixed_offsets[i]);
		}
		else /* Variable-length column */
		{
			cache->fixed_offsets[i] = UINT32_MAX; /* Not a fixed column */
			cache->var_indexes[i] = var_col_index++;
			
			OPTIMIZED_LOG("build_column_cache: Column %d (%s) variable-length, var_index=%d", 
						  i + 1, NameStr(att->attname), cache->var_indexes[i]);
		}
	}
	
	cache->fixed_data_len = current_fixed_offset;
	cache->var_col_count = var_col_index;
	
	OPTIMIZED_LOG("build_column_cache: Cache built - fixed_data_len=%zu, var_col_count=%d", 
				  cache->fixed_data_len, cache->var_col_count);
	
	return cache;
}

/*
 * optimized_extract_attribute_no_cache
 *      Fallback function for cases where we don't have a cache available.
 *      This uses the old O(N) method but only for materialization operations.
 */
static Datum
optimized_extract_attribute_no_cache(HeapTuple tuple, int attnum, TupleDesc tupleDesc, bool *isnull)
{
	/* Build a temporary cache for this operation */
	OptimizedColumnMapCache *temp_cache = build_column_cache(tupleDesc);
	Datum result = optimized_extract_attribute(tuple, attnum, tupleDesc, temp_cache, isnull);
	
	/* Clean up temporary cache */
	pfree(temp_cache->fixed_offsets);
	pfree(temp_cache->var_indexes);
	pfree(temp_cache);
	
	return result;
}

/*
 * optimized_slot_init_cache
 *      Initialize cached data pointers in the slot to reduce memory indirection.
 *      This function calculates commonly accessed pointers once and stores them
 *      in the slot structure for fast access during attribute extraction.
 */
static void
optimized_slot_init_cache(OptimizedTupleTableSlot *opt_slot)
{
	HeapTuple tuple = opt_slot->opt_tuple;
	
	if (!tuple || !tuple->t_data)
	{
		opt_slot->cache_valid = false;
		return;
	}
	
	/* Cache the tuple header */
	opt_slot->cached_header = (OptimizedTupleHeader) tuple->t_data;
	
	/* Calculate and cache commonly accessed data pointers */
	char *data_start = (char *) opt_slot->cached_header + opt_slot->cached_header->t_hoff;
	
	if (HeapTupleHasNulls(tuple))
	{
		/* Skip null bitmap if present */
		int null_bitmap_len = BITMAPLEN(opt_slot->base.tts_tupleDescriptor->natts);
		data_start += MAXALIGN(null_bitmap_len);
	}
	
	/* Cache variable column count and offsets array pointer */
	uint32 *var_col_count_ptr = (uint32 *) MAXALIGN(data_start);
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
static inline Datum
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
 * optimized_extract_attribute
 *      Extract a single attribute from an optimized tuple format.
 *      Uses pre-computed cache for O(1) attribute access performance.
 */
static Datum
optimized_extract_attribute(HeapTuple tuple, int attnum, TupleDesc tupleDesc, 
                           OptimizedColumnMapCache *cache, bool *isnull)
{
    OptimizedTupleHeader header = (OptimizedTupleHeader) tuple->t_data;
    Form_pg_attribute att = TupleDescAttr(tupleDesc, attnum - 1);
    
    /*
     * PERFORMANCE OPTIMIZATION: Fast validation with minimal overhead
     */
    if (cache && cache->fixed_offsets && cache->var_indexes && 
        cache->natts == tupleDesc->natts && attnum >= 1 && attnum <= cache->natts)
    {
        /*
         * FAST PATH: Use cached offsets for O(1) access
         */
        if (att->attlen > 0) /* Fixed-length column */
        {
            /* Fast path for fixed-length columns - most common case */
            uint32 fixed_off = cache->fixed_offsets[attnum - 1];
            
            /* Calculate fixed data pointer with minimal overhead */
            char *data_start = (char *) header + header->t_hoff;
            uint32 *var_col_count_ptr = (uint32 *) MAXALIGN(data_start);
            uint32 var_col_count = *var_col_count_ptr;
            uint32 *var_offsets = (uint32 *) ((char *) var_col_count_ptr + MAXALIGN(sizeof(uint32)));
            char *fixed_data = (char *)var_offsets + MAXALIGN(var_col_count * sizeof(uint32));
            
            char *data_ptr = fixed_data + fixed_off;
            *isnull = false;

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
                        break; /* Fall through to slower path */
                }
            }
            /* For non-byval fixed types, use original logic */
            return PointerGetDatum(data_ptr);
        }
        else /* Variable-length column */
        {
            /* Variable columns: use cached var_index */
            int target_var_index = cache->var_indexes[attnum - 1];
            
            char *data_start = (char *) header + header->t_hoff;
            uint32 *var_col_count_ptr = (uint32 *) MAXALIGN(data_start);
            uint32 var_col_count = *var_col_count_ptr;
            uint32 *var_offsets = (uint32 *) ((char *) var_col_count_ptr + MAXALIGN(sizeof(uint32)));
            
            *isnull = false;
            if (target_var_index < var_col_count)
            {
                uint32 absolute_offset = var_offsets[target_var_index];
                char *var_data_ptr = (char *)header + absolute_offset;
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
     * FALLBACK: Original complex logic for compatibility
     * Only used when cache is invalid or for edge cases
     */
    char *null_bitmap = NULL;
    uint32 *var_col_count_ptr;
    uint32 var_col_count;
    uint32 *var_offsets;
    char *fixed_data;
    char *data_ptr;
    uint32 fixed_off = 0;
    int target_var_index = -1;

    if (cache == NULL || cache->fixed_offsets == NULL || cache->var_indexes == NULL || 
        cache->natts != tupleDesc->natts || attnum < 1 || attnum > cache->natts)
    {
        /*
         * FALLBACK: O(N) computation when cache is unavailable
         * This is slower but ensures correctness
         */
        OPTIMIZED_LOG("optimized_extract_attribute: Cache invalid, using O(N) fallback");
        
        int i;
        int var_col_index = 0;
        
        for (i = 0; i < tupleDesc->natts; i++)
        {
            Form_pg_attribute current_att = TupleDescAttr(tupleDesc, i);
            
            if (current_att->attisdropped)
                continue;
                
            if (current_att->attlen > 0) /* Fixed-length column */
            {
                if (i < attnum - 1)
                    fixed_off += current_att->attlen;
            }
            else /* Variable-length column */
            {
                if (i < attnum - 1)
                    var_col_index++;
                else if (i == attnum - 1)
                    target_var_index = var_col_index;
            }
        }
        
        OPTIMIZED_LOG("optimized_extract_attribute: O(N) computed fixed_off=%u, var_index=%d", fixed_off, target_var_index);
    }
    else
    {
        /*
         * O(1) CACHE LOOKUP: Use pre-computed column positions - FAST PATH!
         */
        if (att->attlen > 0) /* Fixed-length column */
        {
            fixed_off = cache->fixed_offsets[attnum - 1];
            OPTIMIZED_LOG("optimized_extract_attribute: Fixed column, cached offset=%u", fixed_off);
        }
        else /* Variable-length column */
        {
            target_var_index = cache->var_indexes[attnum - 1];
            OPTIMIZED_LOG("optimized_extract_attribute: Variable column, cached var_index=%d", target_var_index);
        }
    }

    /* Set up pointers to data sections */
    if (HeapTupleHasNulls(tuple))
    {
        /* Null bitmap is present */
        null_bitmap = (char *) header + header->t_hoff;
        var_col_count_ptr = (uint32 *) (null_bitmap + MAXALIGN(BITMAPLEN(tupleDesc->natts)));
    }
    else
    {
        /* No null bitmap - variable column count starts after header with proper alignment */
        char *data_start = (char *) header + header->t_hoff;
        var_col_count_ptr = (uint32 *) MAXALIGN(data_start);
    }

    var_col_count = *var_col_count_ptr;
    var_offsets = (uint32 *) ((char *) var_col_count_ptr + MAXALIGN(sizeof(uint32)));

    OPTIMIZED_LOG("optimized_extract_attribute: var_col_count=%u, natts=%d, hasnulls=%d",
         var_col_count, tupleDesc->natts, HeapTupleHasNulls(tuple));

    /* Fixed data starts immediately after the variable offsets array */
    fixed_data = (char *)var_offsets + MAXALIGN(var_col_count * sizeof(uint32));

    OPTIMIZED_LOG("optimized_extract_attribute: fixed_data=%p", fixed_data);

    /* Check if this specific attribute is NULL using the null bitmap */
    if (null_bitmap != NULL)
    {
        int byte_offset = (attnum - 1) / 8;
        int bit_offset = (attnum - 1) % 8;
        
        /* Check if the bit is 0 (NULL) or 1 (not NULL) */
        if (!(null_bitmap[byte_offset] & (1 << bit_offset)))
        {
            *isnull = true;
            return (Datum) 0;  /* Return NULL datum */
        }
    }
    
    *isnull = false;

    /* Handle fixed-length columns */
    if (att->attlen > 0)
    {
        char val;
        int16 val16;
        int32 val32;
#if SIZEOF_DATUM == 8
        Datum val_datum;
#endif
        
        data_ptr = fixed_data + fixed_off;
        OPTIMIZED_LOG("optimized_extract_attribute: fixed column, offset=%u", fixed_off);

        if (att->attbyval)
        {
            /* Pass-by-value: extract the value based on length */
            switch (att->attlen)
            {
                case sizeof(char):
                    val = *((char *) data_ptr);
                    OPTIMIZED_LOG("optimized_extract_attribute: char value=%d", val);
                    return CharGetDatum(val);
                case sizeof(int16):
                    val16 = *((int16 *) data_ptr);
                    OPTIMIZED_LOG("optimized_extract_attribute: int16 value=%d", val16);
                    return Int16GetDatum(val16);
                case sizeof(int32):
                    val32 = *((int32 *) data_ptr);
                    OPTIMIZED_LOG("optimized_extract_attribute: int32 value=%d", val32);
                    return Int32GetDatum(val32);
#if SIZEOF_DATUM == 8
                case sizeof(Datum):
                    val_datum = *((Datum *) data_ptr);
                    OPTIMIZED_LOG("optimized_extract_attribute: Datum value=%ld", val_datum);
                    return val_datum;
#endif
                default:
                    elog(ERROR, "unsupported byval length: %d", att->attlen);
                    return (Datum) 0;
            }
        }
        else
        {
            /* Pass-by-reference: return pointer to the data */
            OPTIMIZED_LOG("optimized_extract_attribute: fixed pass-by-ref, data_ptr=%p", data_ptr);
            return PointerGetDatum(data_ptr);
        }
    }
    /* Handle variable-length columns */
    else
    {
        OPTIMIZED_LOG("optimized_extract_attribute: variable column, var_index=%d", target_var_index);

        /* Get the data from the variable section using the absolute offset */
        if (target_var_index < var_col_count)
        {
            uint32 absolute_offset = var_offsets[target_var_index];
            char *var_data_ptr = (char *)header + absolute_offset;
            OPTIMIZED_LOG("optimized_extract_attribute: absolute offset=%u, var_data_ptr=%p", absolute_offset, var_data_ptr);
            /* Variable-length data is always pass-by-reference */
            return PointerGetDatum(var_data_ptr);
        }
        else
        {
            OPTIMIZED_LOG("optimized_extract_attribute: var_index >= var_col_count, returning NULL");
            *isnull = true;
            return (Datum) NULL;
        }
    }
}

/*
 * optimized_getattr
 *      Extract an attribute of an optimized tuple and return it as a Datum.
 *      This works for either system or user attributes. The given attnum
 *      is properly range-checked.
 *
 *      If the field in question has a NULL value, we return a zero Datum
 *      and set *isnull == true. Otherwise, we set *isnull == false.
 *
 *      <tuple> is the pointer to the optimized tuple. <attnum> is the attribute
 *      number of the column (field) caller wants. <tupleDesc> is a pointer
 *      to the structure describing the row and all its fields.
 */
static Datum
optimized_getattr(HeapTuple tuple, int attnum, TupleDesc tupleDesc, bool *isnull)
{
	Datum datum;
	Form_pg_attribute att;

	att = TupleDescAttr(tupleDesc, attnum - 1);
	datum = heap_getattr(tuple, attnum, tupleDesc, isnull);

	if (!*isnull && att->attlen == -1)
	{
		datum = PointerGetDatum(pg_detoast_datum((struct varlena *) DatumGetPointer(datum)));
	}

	return datum;
}

/*
 * Wrapper function to fit the TupleTableSlotOps.getattr interface.
 * It retrieves the tuple and descriptor from the slot and calls the
 * main optimized_getattr function.
 */
/*
 * Custom getsomeattrs function for on-demand attribute fetching.
 * This is called when the executor needs specific attributes from the slot.
 */
static void
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

static Datum
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
 * choose_offset_encoding
 *      Determine the optimal offset encoding strategy based on tuple characteristics.
 *      Use 16-bit offsets for small tuples to reduce storage overhead.
 */
static OffsetEncodingType
choose_offset_encoding(Size estimated_tuple_size, int var_col_count)
{
    /* Use 16-bit offsets if:
     * 1. Tuple size is small enough (< 32KB)
     * 2. We have variable columns to optimize
     * 3. Offset array would provide meaningful savings
     */
    if (estimated_tuple_size < 32768 && var_col_count > 0 && var_col_count >= 2)
    {
        OPTIMIZED_LOG("Using 16-bit offset encoding for tuple size %zu with %d var columns", 
                     estimated_tuple_size, var_col_count);
        return OFFSET_ENCODING_16BIT;
    }
    
    OPTIMIZED_LOG("Using 32-bit offset encoding for tuple size %zu with %d var columns", 
                 estimated_tuple_size, var_col_count);
    return OFFSET_ENCODING_32BIT;
}

/*
 * optimized_tuple_delete - delete a tuple from an optimized table
 *      This function implements DELETE operations for the optimized row format
 *      by marking the tuple as deleted using PostgreSQL's MVCC mechanism.
 */
static TM_Result
optimized_tuple_delete(Relation relation, ItemPointer tid,
                      CommandId cid, Snapshot crosscheck, Snapshot snapshot,
                      bool wait, TM_FailureData *tmfd, bool changingPart)
{
    TM_Result   result;
    TransactionId xid = GetCurrentTransactionId();
    ItemId      lp;
    HeapTupleData tp;
    Page        page;
    BlockNumber block;
    Buffer      buffer;
    TransactionId new_xmax;
    uint16      new_infomask, new_infomask2;
    bool        iscombo;

    Assert(ItemPointerIsValid(tid));

    OPTIMIZED_LOG("optimized_tuple_delete: deleting tuple at (%u,%u)",
                  ItemPointerGetBlockNumber(tid),
                  ItemPointerGetOffsetNumber(tid));

    /*
     * Forbid this during a parallel operation, lest it allocate a combo CID.
     * Other workers might need that combo CID for visibility checks, and we
     * have no provision for broadcasting it to them.
     */
    if (IsInParallelMode())
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TRANSACTION_STATE),
                 errmsg("cannot delete tuples during a parallel operation")));

    // 1. BUFFER MANAGEMENT: Get the page containing the tuple
    block = ItemPointerGetBlockNumber(tid);
    buffer = ReadBuffer(relation, block);
    page = BufferGetPage(buffer);

    // 2. LOCKING: Acquire exclusive lock on buffer
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

    // 3. TUPLE EXTRACTION: Get tuple from page
    lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
    Assert(ItemIdIsNormal(lp));
    
    tp.t_tableOid = RelationGetRelid(relation);
    tp.t_data = (HeapTupleHeader) PageGetItem(page, lp);
    tp.t_len = ItemIdGetLength(lp);
    tp.t_self = *tid;

    OPTIMIZED_LOG("optimized_tuple_delete: found tuple with xmin=%u, xmax=%u",
                  HeapTupleHeaderGetXmin(tp.t_data),
                  HeapTupleHeaderGetRawXmax(tp.t_data));

    // 4. MVCC COMPLIANCE: Check tuple visibility and update status
    result = HeapTupleSatisfiesUpdate(&tp, cid, buffer);
    
    if (result == TM_Invisible)
    {
        UnlockReleaseBuffer(buffer);
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("attempted to delete invisible tuple")));
    }
    
    if (result != TM_Ok)
    {
        UnlockReleaseBuffer(buffer);
        OPTIMIZED_LOG("optimized_tuple_delete: tuple not available for deletion, result=%d", result);
        return result; // Tuple being modified, deleted, etc.
    }

    // 5. DELETION LOGIC: Mark tuple as deleted
    new_xmax = xid;
    new_infomask = tp.t_data->t_infomask;
    new_infomask2 = tp.t_data->t_infomask2;
    iscombo = false;
    
    // Compute new infomask bits for deletion (simplified version)
    new_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
    new_infomask2 &= ~HEAP_KEYS_UPDATED;
    new_infomask |= HEAP_XMAX_EXCL_LOCK;

    START_CRIT_SECTION();

    // 6. ATOMIC UPDATE: Set deletion markers
    tp.t_data->t_infomask = new_infomask;
    tp.t_data->t_infomask2 = new_infomask2;
    HeapTupleHeaderClearHotUpdated(tp.t_data);
    HeapTupleHeaderSetXmax(tp.t_data, new_xmax);
    HeapTupleHeaderSetCmax(tp.t_data, cid, iscombo);

    MarkBufferDirty(buffer);

    OPTIMIZED_LOG("optimized_tuple_delete: marked tuple as deleted with xmax=%u", new_xmax);

    // 7. BASIC WAL LOGGING: Simplified version for initial implementation
    // TODO: Add full WAL logging in production version
    if (RelationNeedsWAL(relation))
    {
        OPTIMIZED_LOG("optimized_tuple_delete: WAL logging would be performed here");
        // Simplified: Mark buffer dirty but skip detailed WAL for now
    }

    END_CRIT_SECTION();

    LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
    
    // 8. CLEANUP AND STATISTICS
    ReleaseBuffer(buffer);

    pgstat_count_heap_delete(relation);

    OPTIMIZED_LOG("optimized_tuple_delete: successfully deleted tuple");

    return TM_Ok;
}

/*
 * optimized_tuple_update - update a tuple in an optimized table
 *      This function implements UPDATE operations for the optimized row format
 *      using a simplified "delete and insert" approach without HOT updates.
 */
static TM_Result
optimized_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
                      CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait,
                      TM_FailureData *tmfd, LockTupleMode *lockmode,
                      TU_UpdateIndexes *update_indexes)
{
    TM_Result   result;
    TransactionId xid = GetCurrentTransactionId();
    ItemId      old_lp;
    HeapTupleData oldtup;
    Page        page;
    BlockNumber block;
    Buffer      buffer;
    TransactionId new_xmax;
    uint16      new_infomask, new_infomask2;
    bool        iscombo;
    
    /* New tuple data */
    HeapTuple   newtup;
    Buffer      newbuf;
    Page        newpage;
    OffsetNumber newoffnum;
    ItemPointerData newtid;

    Assert(ItemPointerIsValid(otid));

    OPTIMIZED_LOG("optimized_tuple_update: updating tuple at (%u,%u)",
                  ItemPointerGetBlockNumber(otid),
                  ItemPointerGetOffsetNumber(otid));

    /*
     * Forbid this during a parallel operation, lest it allocate a combo CID.
     */
    if (IsInParallelMode())
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TRANSACTION_STATE),
                 errmsg("cannot update tuples during a parallel operation")));

    // 1. BUFFER MANAGEMENT: Get the page containing the old tuple
    block = ItemPointerGetBlockNumber(otid);
    buffer = ReadBuffer(relation, block);
    page = BufferGetPage(buffer);

    // 2. LOCKING: Acquire exclusive lock on buffer
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

    // 3. OLD TUPLE EXTRACTION: Get old tuple from page
    old_lp = PageGetItemId(page, ItemPointerGetOffsetNumber(otid));
    Assert(ItemIdIsNormal(old_lp));
    
    oldtup.t_tableOid = RelationGetRelid(relation);
    oldtup.t_data = (HeapTupleHeader) PageGetItem(page, old_lp);
    oldtup.t_len = ItemIdGetLength(old_lp);
    oldtup.t_self = *otid;

    OPTIMIZED_LOG("optimized_tuple_update: found old tuple with xmin=%u, xmax=%u",
                  HeapTupleHeaderGetXmin(oldtup.t_data),
                  HeapTupleHeaderGetRawXmax(oldtup.t_data));

    // 4. MVCC COMPLIANCE: Check old tuple visibility
    result = HeapTupleSatisfiesUpdate(&oldtup, cid, buffer);
    
    if (result == TM_Invisible)
    {
        UnlockReleaseBuffer(buffer);
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("attempted to update invisible tuple")));
    }
    
    if (result != TM_Ok)
    {
        UnlockReleaseBuffer(buffer);
        OPTIMIZED_LOG("optimized_tuple_update: old tuple not available for update, result=%d", result);
        return result; // Tuple being modified, deleted, etc.
    }

    // 5. NEW TUPLE PREPARATION: Estimate size for new tuple (simplified approach)
    // For this basic implementation, we'll estimate a size similar to the old tuple
    // In a production version, this would properly calculate the optimized tuple size
    Size estimated_tuple_size = oldtup.t_len * 2; // Conservative estimate
    
    OPTIMIZED_LOG("optimized_tuple_update: estimated new tuple size %zu bytes", estimated_tuple_size);

    // 6. NEW TUPLE PLACEMENT: Find space for the new tuple
    // For simplicity, always use a new page (no HOT updates in this basic version)
    newbuf = RelationGetBufferForTuple(relation, estimated_tuple_size,
                                      InvalidBuffer, 0, NULL, NULL, NULL, 0);
    if (!BufferIsValid(newbuf))
    {
        UnlockReleaseBuffer(buffer);
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to get buffer for new tuple during update")));
    }

    newpage = BufferGetPage(newbuf);
    LockBuffer(newbuf, BUFFER_LOCK_EXCLUSIVE);

    START_CRIT_SECTION();

    // 7. OLD TUPLE UPDATE: Mark old tuple as updated
    new_xmax = xid;
    new_infomask = oldtup.t_data->t_infomask;
    new_infomask2 = oldtup.t_data->t_infomask2;
    iscombo = false;
    
    // Compute new infomask bits for update (simplified version)
    new_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
    new_infomask2 &= ~HEAP_KEYS_UPDATED;
    new_infomask |= HEAP_XMAX_EXCL_LOCK;

    // Mark old tuple as updated
    oldtup.t_data->t_infomask = new_infomask;
    oldtup.t_data->t_infomask2 = new_infomask2;
    HeapTupleHeaderClearHotUpdated(oldtup.t_data);
    HeapTupleHeaderSetXmax(oldtup.t_data, new_xmax);
    HeapTupleHeaderSetCmax(oldtup.t_data, cid, iscombo);

    // 8. NEW TUPLE INSERTION: For simplicity, delegate to optimized_tuple_insert
    // This is not the most efficient approach but works for basic functionality
    // A production implementation would build the tuple directly here
    
    // Temporarily release the newbuf lock and call insert (it will handle the tuple creation)
    LockBuffer(newbuf, BUFFER_LOCK_UNLOCK);
    ReleaseBuffer(newbuf);
    
    // Call our existing insert function to handle the new tuple creation
    // Note: This will get a new buffer, but it's simpler for this basic implementation
    TupleTableSlot *temp_slot = slot; // Use the same slot
    optimized_tuple_insert(relation, temp_slot, GetCurrentCommandId(true), 0, NULL);
    
    // Get the new TID from the slot
    newtid = temp_slot->tts_tid;
    newoffnum = ItemPointerGetOffsetNumber(&newtid);

    // 9. TUPLE CHAINING: Link old tuple to new tuple via ctid (simplified)
    oldtup.t_data->t_ctid = newtid;

    // Mark the original buffer dirty
    MarkBufferDirty(buffer);

    OPTIMIZED_LOG("optimized_tuple_update: marked old tuple as updated with xmax=%u, new tuple at (%u,%u)",
                  new_xmax, ItemPointerGetBlockNumber(&newtid), newoffnum);

    // 10. BASIC WAL LOGGING: Simplified version for initial implementation
    // TODO: Add full WAL logging in production version
    if (RelationNeedsWAL(relation))
    {
        OPTIMIZED_LOG("optimized_tuple_update: WAL logging would be performed here");
        // Simplified: Mark buffer dirty but skip detailed WAL for now
    }

    END_CRIT_SECTION();

    LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
    
    // 11. UPDATE SLOT: Update slot with new tuple information
    slot->tts_tid = newtid;
    slot->tts_tableOid = RelationGetRelid(relation);

    // 12. CLEANUP AND STATISTICS
    ReleaseBuffer(buffer);

    pgstat_count_heap_update(relation, false, false); // false = not HOT update, false = same page

    OPTIMIZED_LOG("optimized_tuple_update: successfully updated tuple");

    return TM_Ok;
}

/*
 * optimized_tuple_insert - insert tuple into an optimized table
 *
 * This is a simplified version of heap_insert() that focuses on the core
 * tuple insertion functionality. It skips:
 * - Parallelism
 * - Toasting
 * - Visibility map updates
 * - Index updates
 * - WAL logging
 */
static void
optimized_tuple_insert(Relation relation, TupleTableSlot *slot,
                      CommandId cid, int options, struct BulkInsertStateData *bistate)
{
    OptimizedTupleHeader header;
    HeapTuple tuple;
    Size len;
    Size fixed_data_len = 0;
    Size var_data_len = 0;
    int var_col_count = 0;
    int i;
    TupleDesc tupdesc = RelationGetDescr(relation);
    Buffer buffer;
    Buffer vmbuffer = InvalidBuffer;
    Buffer vmbuffer_other = InvalidBuffer;
    Page page;
    OffsetNumber offnum;
    char *fixed_data;
    char *var_data;
    uint32 *var_offsets;
    int fixed_pos = 0;
    int var_pos = 0;
    char *null_bitmap = NULL;
    ItemPointerData InvalidItemPointer;
    int var_col_index = 0;  /* Track which variable column we're processing */
    bool hasnull = false;
    bool *isnull_array;
    Datum *values_array;
    uint32 *var_col_count_ptr;
    Size base_offset;
    Size varlen;

    OPTIMIZED_LOG("Starting optimized tuple insert for relation %s",
                  RelationGetRelationName(relation));

    /* Pre-allocate arrays to check for nulls */
    isnull_array = (bool *) palloc0(tupdesc->natts * sizeof(bool));
    values_array = (Datum *) palloc(tupdesc->natts * sizeof(Datum));

    /* Get the heap tuple from slot for direct extraction */
    HeapTuple heap_tuple = ExecFetchSlotHeapTuple(slot, false, NULL);
    
    /* First pass: Check for nulls and count columns */
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute att = TupleDescAttr(tupdesc, i);
        if (!att->attisdropped)
        {
            /* Use heap_getattr to extract from heap format data */
            values_array[i] = heap_getattr(heap_tuple, i + 1, tupdesc, &isnull_array[i]);
            if (isnull_array[i])
            {
                hasnull = true;
                OPTIMIZED_LOG("Column %d (%s) is NULL", i + 1, NameStr(att->attname));
            }
            else if (att->attlen > 0)
            {
                fixed_data_len += att->attlen;
            }
            else
            {
                /* Only count variable-length columns that are not NULL */
                if (!isnull_array[i])
                {
                    var_col_count++;
                }
                /* We'll calculate actual variable data length in second pass */
            }
        }
    }

    OPTIMIZED_LOG("Tuple analysis: hasnull=%d, fixed_data_len=%zu, var_col_count=%d",
                  hasnull, fixed_data_len, var_col_count);

    /* Calculate total length needed */
    len = SizeofOptimizedTupleHeader;
    len = MAXALIGN(len);  /* Align header */

    /* Add space for null bitmap only if there are nulls */
    if (hasnull)
    {
        len += BITMAPLEN(tupdesc->natts);
        len = MAXALIGN(len);  /* Align null bitmap */
        OPTIMIZED_LOG("Added null bitmap space: %d bytes", BITMAPLEN(tupdesc->natts));
    }

    /* Add space for variable column count (uint32) */
    len += sizeof(uint32);
    len = MAXALIGN(len);  /* Align var column count */

    /* STORAGE OPTIMIZATION: Choose offset encoding based on estimated tuple size */
    OffsetEncodingType offset_encoding = choose_offset_encoding(len + var_data_len, var_col_count);
    Size offset_size = (offset_encoding == OFFSET_ENCODING_16BIT) ? sizeof(uint16) : sizeof(uint32);
    
    /* Add space for variable offsets array with optimized encoding */
    Size var_offsets_size = MAXALIGN(var_col_count * offset_size);
    len += var_offsets_size;

    /* Add space for fixed-length columns */
    len += MAXALIGN(fixed_data_len);

    /* Calculate variable data length by examining the slot */
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute att = TupleDescAttr(tupdesc, i);
        if (att->attisdropped || att->attlen > 0)
            continue;

        if (!isnull_array[i])
        {
            if (att->attlen == -1)  /* varlena */
            {
                Pointer varlena_ptr = DatumGetPointer(values_array[i]);
                Size varlena_size;
                
                /* Safety check to prevent invalid memory access */
                if (varlena_ptr == NULL)
                {
                    OPTIMIZED_LOG("ERROR: NULL pointer for varlena column %d (%s)", 
                                  i + 1, NameStr(att->attname));
                    elog(ERROR, "NULL pointer encountered for varlena column %s", 
                         NameStr(att->attname));
                }
                
                varlena_size = VARSIZE_ANY(varlena_ptr);
                
                /* Sanity check for reasonable varlena size */
                if (varlena_size > MaxAllocSize || varlena_size < 1)
                {
                    OPTIMIZED_LOG("ERROR: Invalid varlena size %zu for column %d (%s)", 
                                  varlena_size, i + 1, NameStr(att->attname));
                    elog(ERROR, "Invalid varlena size %zu for column %s (expected 1 to %zu)",
                         varlena_size, NameStr(att->attname), MaxAllocSize);
                }
                
                var_data_len += varlena_size;
                OPTIMIZED_LOG("Column %d (%s): varlena size=%zu, total_var_data_len=%zu", 
                              i + 1, NameStr(att->attname), varlena_size, var_data_len);
            }
            else  /* cstring */
            {
                char *cstring_ptr = DatumGetCString(values_array[i]);
                Size cstring_len;
                
                /* Safety check for cstring */
                if (cstring_ptr == NULL)
                {
                    OPTIMIZED_LOG("ERROR: NULL pointer for cstring column %d (%s)", 
                                  i + 1, NameStr(att->attname));
                    elog(ERROR, "NULL pointer encountered for cstring column %s", 
                         NameStr(att->attname));
                }
                
                cstring_len = strlen(cstring_ptr) + 1;
                
                /* Sanity check for reasonable cstring length */
                if (cstring_len > MaxAllocSize)
                {
                    OPTIMIZED_LOG("ERROR: Invalid cstring length %zu for column %d (%s)", 
                                  cstring_len, i + 1, NameStr(att->attname));
                    elog(ERROR, "Invalid cstring length %zu for column %s",
                         cstring_len, NameStr(att->attname));
                }
                
                var_data_len += cstring_len;
                OPTIMIZED_LOG("Column %d (%s): cstring length=%zu, total_var_data_len=%zu", 
                              i + 1, NameStr(att->attname), cstring_len, var_data_len);
            }
        }
    }

    /* Add space for variable-length columns */
    len += MAXALIGN(var_data_len);

    OPTIMIZED_LOG("Final tuple length: %zu bytes (var_data_len=%zu)", len, var_data_len);
    
    /* STORAGE ANALYSIS: Compare with heap tuple size for storage efficiency analysis */
    {
        Size heap_tuple_size = heap_tuple->t_len;
        Size optimized_tuple_size = len;
        Size overhead = optimized_tuple_size - heap_tuple_size;
        float overhead_percent = ((float)overhead / heap_tuple_size) * 100.0f;
        
        /* Calculate component sizes for detailed analysis */
        Size header_size = SizeofOptimizedTupleHeader;
        Size null_bitmap_size = hasnull ? MAXALIGN(BITMAPLEN(tupdesc->natts)) : 0;
        Size var_count_size = MAXALIGN(sizeof(uint32));
        Size var_offsets_analysis_size = var_offsets_size;  /* Use calculated size with encoding */
        Size fixed_data_size = MAXALIGN(fixed_data_len);
        Size var_data_size = MAXALIGN(var_data_len);
        
        OPTIMIZED_LOG("STORAGE COMPARISON:");
        OPTIMIZED_LOG("  Heap tuple size: %zu bytes", heap_tuple_size);
        OPTIMIZED_LOG("  Optimized tuple size: %zu bytes", optimized_tuple_size);
        OPTIMIZED_LOG("  Storage overhead: %zu bytes (%.1f%%)", overhead, overhead_percent);
        OPTIMIZED_LOG("COMPONENT BREAKDOWN:");
        OPTIMIZED_LOG("  Header: %zu bytes", header_size);
        OPTIMIZED_LOG("  Null bitmap: %zu bytes", null_bitmap_size);
        OPTIMIZED_LOG("  Var count: %zu bytes", var_count_size);
        OPTIMIZED_LOG("  Var offsets array: %zu bytes (for %d vars, %s encoding)", 
                      var_offsets_analysis_size, var_col_count, 
                      (offset_encoding == OFFSET_ENCODING_16BIT) ? "16-bit" : "32-bit");
        OPTIMIZED_LOG("  Fixed data: %zu bytes", fixed_data_size);
        OPTIMIZED_LOG("  Variable data: %zu bytes", var_data_size);
        OPTIMIZED_LOG("  Total calculated: %zu bytes", 
                      header_size + null_bitmap_size + var_count_size + var_offsets_analysis_size + fixed_data_size + var_data_size);
        
        /* Identify major overhead sources */
        if (var_offsets_analysis_size > 0) {
            float offset_overhead_percent = ((float)var_offsets_analysis_size / heap_tuple_size) * 100.0f;
            OPTIMIZED_LOG("  Offset array overhead: %.1f%% of heap size", offset_overhead_percent);
        }
        
        /* Calculate alignment waste */
        Size unaligned_total = header_size + (hasnull ? BITMAPLEN(tupdesc->natts) : 0) + 
                              sizeof(uint32) + (var_col_count * sizeof(uint32)) + 
                              fixed_data_len + var_data_len;
        Size alignment_waste = optimized_tuple_size - unaligned_total;
        float alignment_waste_percent = ((float)alignment_waste / heap_tuple_size) * 100.0f;
        OPTIMIZED_LOG("  Alignment waste: %zu bytes (%.1f%% of heap size)", alignment_waste, alignment_waste_percent);
    }

    /* Final sanity check for total tuple length */
    if (len > MaxAllocSize || len < SizeofOptimizedTupleHeader)
    {
        OPTIMIZED_LOG("ERROR: Invalid final tuple length %zu (expected %zu to %zu)", 
                      len, SizeofOptimizedTupleHeader, MaxAllocSize);
        elog(ERROR, "Invalid final tuple length %zu for relation %s (var_data_len=%zu, fixed_data_len=%zu)",
             len, RelationGetRelationName(relation), var_data_len, fixed_data_len);
    }

	ItemPointerSetInvalid(&InvalidItemPointer);

    /* Allocate the tuple */
    tuple = (HeapTuple) palloc0(HEAPTUPLESIZE + len);
    tuple->t_len = len;
    tuple->t_self = InvalidItemPointer;
    tuple->t_tableOid = RelationGetRelid(relation);
    tuple->t_data = (HeapTupleHeader) ((char *) tuple + HEAPTUPLESIZE);

    /* Initialize the header */
    header = (OptimizedTupleHeader) tuple->t_data;
    HeapTupleHeaderSetDatumLength(header, len);
    header->t_infomask = 0;  /* Initialize to 0 first */
    if (hasnull)
        header->t_infomask |= HEAP_HASNULL;  /* Only set if there are nulls */
    header->t_infomask2 = 0;  /* Initialize to 0 first */
    if (offset_encoding == OFFSET_ENCODING_16BIT)
        header->t_infomask2 |= OPTIMIZED_OFFSET_16BIT;  /* Set 16-bit offset flag */
    HeapTupleHeaderSetNatts(header, tupdesc->natts);  /* Set natts properly */
    header->t_hoff = SizeofOptimizedTupleHeader;

	/* Set transaction information */
	HeapTupleHeaderSetXmin(header, GetCurrentTransactionId());
	HeapTupleHeaderSetCmin(header, GetCurrentCommandId(true));
	HeapTupleHeaderSetXmax(header, 0); /* for now */
	ItemPointerSetInvalid(&header->t_ctid);

    OPTIMIZED_LOG("Header initialized: t_infomask=0x%x, t_hoff=%u",
                  header->t_infomask, header->t_hoff);

    /* Set up pointers to data sections */
    if (hasnull)
    {
        null_bitmap = (char *) header + header->t_hoff;
        var_col_count_ptr = (uint32 *) (null_bitmap + MAXALIGN(BITMAPLEN(tupdesc->natts)));
        var_offsets = (uint32 *) ((char *)var_col_count_ptr + MAXALIGN(sizeof(uint32)));

        /* Initialize null bitmap to all zeros */
        memset(null_bitmap, 0, BITMAPLEN(tupdesc->natts));

        /* Store the variable column count */
        *var_col_count_ptr = var_col_count;
    }
    else
    {
        /* No null bitmap - ensure proper alignment for variable column count */
        char *data_start = (char *) header + header->t_hoff;
        var_col_count_ptr = (uint32 *) MAXALIGN(data_start);
        var_offsets = (uint32 *) ((char *)var_col_count_ptr + MAXALIGN(sizeof(uint32)));

        /* Store the variable column count */
        *var_col_count_ptr = var_col_count;
    }

    fixed_data = (char *) (var_offsets) + MAXALIGN(var_col_count * sizeof(uint32));
    var_data = fixed_data + MAXALIGN(fixed_data_len);

    OPTIMIZED_LOG("Data pointers: fixed_data=%p, var_data=%p", fixed_data, var_data);

    /* Calculate the base offset for absolute positioning */
    base_offset = (char *)fixed_data - (char *)header;

    /* Second pass: Copy data into reorganized layout */
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute att = TupleDescAttr(tupdesc, i);
        Datum value = values_array[i];
        bool isnull = isnull_array[i];

        if (att->attisdropped)
            continue;

        if (hasnull && null_bitmap != NULL)
        {
            if (isnull)
            {
                /* Bit is already 0 from memset, so nothing to do */
            }
            else
            {
                /* Set the bit (1 = not null) */
                null_bitmap[i >> 3] |= (1 << (i & 0x07));
            }
        }

        if (isnull)
            continue;

        if (att->attlen > 0)
        {
            /* Fixed-length column - copy based on data type */
            if (att->attbyval)
            {
                /* Pass-by-value: store the datum directly */
                store_att_byval(fixed_data + fixed_pos, value, att->attlen);
            }
            else
            {
                /* Pass-by-reference: copy the data */
                memcpy(fixed_data + fixed_pos, DatumGetPointer(value), att->attlen);
            }
            fixed_pos += att->attlen;
        }
        else
        {
            /* Variable-length column - store absolute offset and copy data */
            if (att->attlen == -1)  /* varlena */
            {
                varlen = VARSIZE_ANY(DatumGetPointer(value));
                memcpy(var_data + var_pos, DatumGetPointer(value), varlen);
            }
            else  /* cstring */
            {
                varlen = strlen(DatumGetCString(value)) + 1;
                memcpy(var_data + var_pos, DatumGetCString(value), varlen);
            }

                    /* Store absolute offset from the start of tuple data with encoding support */
        uint32 absolute_offset = base_offset + MAXALIGN(fixed_data_len) + var_pos;
        
        if (offset_encoding == OFFSET_ENCODING_16BIT)
        {
            /* Use 16-bit offsets for space efficiency */
            if (absolute_offset > 65535)
            {
                OPTIMIZED_LOG("WARNING: Offset %u exceeds 16-bit limit, falling back to 32-bit", absolute_offset);
                /* This shouldn't happen with our size estimation, but handle gracefully */
                elog(ERROR, "Offset %u exceeds 16-bit limit in optimized tuple", absolute_offset);
            }
            ((uint16 *)var_offsets)[var_col_index] = (uint16)absolute_offset;
        }
        else
        {
            /* Use standard 32-bit offsets */
            var_offsets[var_col_index] = absolute_offset;
        }

            var_pos += varlen;
            var_col_index++;
        }
    }

    OPTIMIZED_LOG("Data copy completed: fixed_pos=%d, var_pos=%d", fixed_pos, var_pos);

    /* Free the temporary arrays */
    pfree(isnull_array);
    pfree(values_array);

    /* Get a buffer to insert the tuple */
    buffer = RelationGetBufferForTuple(relation, len, InvalidBuffer, options,
                                     bistate, &vmbuffer, &vmbuffer_other, 1);

    /* Get the page from the buffer */
    page = BufferGetPage(buffer);

    /* Release visibility map pins if we got them */
    if (BufferIsValid(vmbuffer))
        ReleaseBuffer(vmbuffer);
    if (BufferIsValid(vmbuffer_other))
        ReleaseBuffer(vmbuffer_other);

    /* Insert the tuple */
    offnum = PageAddItem(page, (Item) tuple->t_data, len, InvalidOffsetNumber, false, true);
    if (offnum == InvalidOffsetNumber)
        elog(ERROR, "failed to add tuple to page");

    /* Update tuple's self pointer */
    ItemPointerSet(&tuple->t_self, BufferGetBlockNumber(buffer), offnum);
	/* Update the on-page tuple's ctid */
	{
		HeapTupleHeader pght;
		                pght = (HeapTupleHeader)PageGetItem(page, PageGetItemId(page, offnum));
		pght->t_ctid = tuple->t_self;
	}

    OPTIMIZED_LOG("Tuple inserted successfully: block=%u, offset=%u",
                  BufferGetBlockNumber(buffer), offnum);

    /* Mark the buffer dirty */
    MarkBufferDirty(buffer);

    /* Release the buffer */
    UnlockReleaseBuffer(buffer);

    /* Free the tuple */
    pfree(tuple);
}

static void __attribute__((unused))
optimized_tuple_insert_speculative(Relation relation, TupleTableSlot *slot,
                                 CommandId cid, int options, struct BulkInsertStateData *bistate,
                                 uint32 specToken)
{
    /* TODO: Implement speculative insert */
}

static void __attribute__((unused))
optimized_tuple_complete_speculative(Relation relation, TupleTableSlot *slot,
                                   uint32 specToken, bool succeeded)
{
    /* TODO: Implement speculative insert completion */
}

static bool
optimized_relation_needs_toast_table(Relation rel)
{
	/* For now, disable TOAST tables for this AM */
	return false;
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
		TupleDesc tupdesc = slot->tts_tupleDescriptor;
		int natts = tupdesc->natts;
		Datum *values = (Datum *) palloc0(natts * sizeof(Datum));
		bool *isnull = (bool *) palloc0(natts * sizeof(bool));
		
		/* Extract each attribute from optimized format */
		for (int i = 0; i < natts; i++)
		{
			values[i] = optimized_extract_attribute_no_cache(hslot->tuple, i + 1, tupdesc, &isnull[i]);
			
			/* Handle detoasting for variable-length attributes */
			if (!isnull[i] && tupdesc->attrs[i].attlen == -1)
			{
				values[i] = PointerGetDatum(pg_detoast_datum((struct varlena *) DatumGetPointer(values[i])));
			}
		}
		
		/* Create a new standard heap tuple from the extracted values */
		HeapTuple heap_tuple = heap_form_tuple(tupdesc, values, isnull);
		
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

	oldcontext = MemoryContextSwitchTo(dstslot->tts_mcxt);
	tuple = ExecCopySlotHeapTuple(srcslot);
	MemoryContextSwitchTo(oldcontext);

	/*
	 * Manually store the tuple to preserve custom slot operations.
	 * Using ExecStoreHeapTuple would fail with "trying to store a heap tuple 
	 * into wrong type of slot" because it checks TTS_IS_HEAPTUPLE.
	 */
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) dstslot;
	
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
	bool is_optimized_table = false;
	if (hslot->tuple && hslot->tuple->t_tableOid != InvalidOid)
	{
		Relation rel = try_relation_open(hslot->tuple->t_tableOid, NoLock);
		if (rel != NULL)
		{
			/* Check if the table uses a non-heap access method (i.e., our optimized AM) */
			if (rel->rd_tableam != get_heap_am_routine())
			{
				is_optimized_table = true;
			}
			relation_close(rel, NoLock);
		}
	}
	
	if (is_optimized_table)
	{
		/* This tuple is from an optimized table - convert to standard heap format */
		TupleDesc tupdesc = slot->tts_tupleDescriptor;
		int natts = tupdesc->natts;
		Datum *values = (Datum *) palloc0(natts * sizeof(Datum));
		bool *isnull = (bool *) palloc0(natts * sizeof(bool));
		
		/* Extract each attribute from optimized format */
		for (int i = 0; i < natts; i++)
		{
			values[i] = optimized_extract_attribute_no_cache(hslot->tuple, i + 1, tupdesc, &isnull[i]);
			
			/* Handle detoasting for variable-length attributes */
			if (!isnull[i] && tupdesc->attrs[i].attlen == -1)
			{
				values[i] = PointerGetDatum(pg_detoast_datum((struct varlena *) DatumGetPointer(values[i])));
			}
		}
		
		/* Create a new standard heap tuple from the extracted values */
		HeapTuple heap_tuple = heap_form_tuple(tupdesc, values, isnull);
		
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
	
	Assert(!TTS_EMPTY(slot));
	if (!hslot->tuple)
		optimized_slot_materialize(slot);
	
	/*
	 * Same logic as get_heap_tuple: check if tuple is from optimized table.
	 */
	bool is_optimized_table = false;
	if (hslot->tuple && hslot->tuple->t_tableOid != InvalidOid)
	{
		Relation rel = try_relation_open(hslot->tuple->t_tableOid, NoLock);
		if (rel != NULL)
		{
			/* Non-heap AM implies our optimized AM for this table */
			if (rel->rd_tableam != get_heap_am_routine())
			{
				is_optimized_table = true;
			}
			relation_close(rel, NoLock);
		}
	}
	
	if (is_optimized_table)
	{
		/* Convert optimized format tuple to standard heap format, then copy */
		HeapTuple converted = optimized_slot_get_heap_tuple(slot);
		return heap_copytuple(converted);
	}
	
	return heap_copytuple(hslot->tuple);
}

static MinimalTuple optimized_slot_copy_minimal_tuple(TupleTableSlot *slot)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;
	MemoryContext oldcontext;
	
	Assert(!TTS_EMPTY(slot));
	
	oldcontext = MemoryContextSwitchTo(slot->tts_mcxt);
	
	TupleDesc tupdesc = slot->tts_tupleDescriptor;
	int natts = tupdesc->natts;
	Datum *values = (Datum *) palloc0(natts * sizeof(Datum));
	bool *isnull = (bool *) palloc0(natts * sizeof(bool));
	
	/* Extract each attribute from optimized format */
	for (int i = 0; i < natts; i++)
	{
		values[i] = optimized_extract_attribute_no_cache(hslot->tuple, i + 1, tupdesc, &isnull[i]);
		
		/* Handle detoasting for variable-length attributes */
		if (!isnull[i] && tupdesc->attrs[i].attlen == -1)
		{
			values[i] = PointerGetDatum(pg_detoast_datum((struct varlena *) DatumGetPointer(values[i])));
		}
	}
	
	/* Form a proper MinimalTuple in the slot memory context */
	MinimalTuple mtup = heap_form_minimal_tuple(tupdesc, values, isnull);
	
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
 */
static const TupleTableSlotOps *
optimized_slot_callbacks(Relation relation)
{
	/*
	 * CRITICAL: Use custom slot operations only for table scans to enable projection.
	 * For INSERT operations, we must use standard heap slot operations to avoid
	 * segmentation faults during tuple insertion.
	 * 
	 * Unfortunately, PostgreSQL doesn't provide context about the operation type
	 * in this callback, so we need to use heap operations for all cases to ensure
	 * INSERT stability. The projection optimization will be implemented differently.
	 */
	//return &TTSOpsHeapTuple;

    // Return our custom slot operations instead of heap operations
    return &TTSOpsOptimized; 
}

/* --- Index Scan Implementation --- */

static IndexFetchTableData *
optimized_index_fetch_begin(Relation rel)
{
	const TableAmRoutine *heap_am = get_heap_am_routine();
	return heap_am->index_fetch_begin(rel);
}

static void
optimized_index_fetch_reset(IndexFetchTableData *scan)
{
	const TableAmRoutine *heap_am = get_heap_am_routine();
	heap_am->index_fetch_reset(scan);
}

static void
optimized_index_fetch_end(IndexFetchTableData *scan)
{
	const TableAmRoutine *heap_am = get_heap_am_routine();
	heap_am->index_fetch_end(scan);
}

static bool
optimized_index_fetch_tuple(struct IndexFetchTableData *scan,
                              ItemPointer tid,
                              Snapshot snapshot,
                              TupleTableSlot *slot,
                              bool *call_again, bool *all_dead)
{
	const TableAmRoutine *heap_am = get_heap_am_routine();
	return heap_am->index_fetch_tuple(scan, tid, snapshot, slot, call_again, all_dead);
}