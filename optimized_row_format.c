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
 */
#define OPTIMIZED_LOG(fmt, ...) \
    /* Debug logging disabled for production */

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
    /* Caching is disabled for now to fix projection, will be re-enabled. */
    /* OptimizedColumnMapCache *column_cache; */
} OptimizedScanDescData;

typedef OptimizedScanDescData *OptimizedScanDesc;

/* Get the heap AM routine to delegate operations to */
static const TableAmRoutine *
get_heap_am_routine(void)
{
    return GetHeapamTableAmRoutine();
}

/* Function declarations */
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
static Datum optimized_extract_attribute(HeapTuple tuple, int attnum, TupleDesc tupleDesc, bool *isnull);
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
/* Caching is disabled for now to fix projection. */
/* static OptimizedColumnMapCache *build_column_cache(TupleDesc tupleDesc); */

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

    /* Caching is disabled for now to fix projection. */
    /*
    if (rel->rd_amcache == NULL)
    {
        rel->rd_amcache = build_column_cache(tupleDesc);
        OPTIMIZED_LOG("optimized_scan_begin: built new cache for relation %s",
                     RelationGetRelationName(rel));
    }
    oscan->column_cache = (OptimizedColumnMapCache *) rel->rd_amcache;
    */

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
					 * TEMPORARY: Revert to eager deformation to fix segfault.
					 * We'll implement projection optimization more carefully in the next iteration.
					 * The key insight is that we need to materialize a standard heap tuple
					 * for executor compatibility while still storing our optimized format.
					 */
					
					/* Clear the slot first */
					ExecClearTuple(slot);
					
					TupleDesc tupdesc = scan->rs_rd->rd_att;
					int natts = tupdesc->natts;
					Datum *values = (Datum *) palloc0(natts * sizeof(Datum));
					bool *isnull = (bool *) palloc0(natts * sizeof(bool));
					
					/* Extract all attributes from optimized format */
					for (int i = 0; i < natts; i++)
					{
						values[i] = optimized_extract_attribute(tuple, i + 1, tupdesc, &isnull[i]);
					}
					
					/* Create a standard heap tuple from the extracted values */
					HeapTuple htup = heap_form_tuple(tupdesc, values, isnull);
					pfree(values);
					pfree(isnull);
					
					/* Store the standard heap tuple in the slot */
					ExecStoreHeapTuple(htup, slot, true);

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
 * NOTE: Caching is temporarily disabled to implement on-demand attribute
 * fetching (projection). It will be re-enabled in a future step to
 * restore O(1) attribute access performance.
 */

/*
 * optimized_extract_attribute
 *      Extract a single attribute from an optimized tuple format.
 *      NOTE: This version computes offsets on-the-fly (O(N) complexity).
 *      Caching will be re-introduced later to make this O(1).
 */
static Datum
optimized_extract_attribute(HeapTuple tuple, int attnum, TupleDesc tupleDesc, bool *isnull)
{
    OptimizedTupleHeader header = (OptimizedTupleHeader) tuple->t_data;
    Form_pg_attribute att = TupleDescAttr(tupleDesc, attnum - 1);
    char *null_bitmap = NULL;
    uint32 *var_col_count_ptr;
    uint32 var_col_count;
    uint32 *var_offsets;
    char *fixed_data;
    char *data_ptr;

    uint32 fixed_len_total = 0;
    uint32 fixed_off = 0;
    int var_att_count = 0;
    int target_var_index = -1;
    int i;

    OPTIMIZED_LOG("optimized_extract_attribute: attnum=%d, attname=%s, attlen=%d",
         attnum, NameStr(att->attname), att->attlen);

    /*
     * Calculate the physical position of the target attribute on-the-fly.
     * We iterate through attributes to find the offset of the desired fixed-width
     * column or the index of the desired variable-width column.
     */
    for (i = 0; i < tupleDesc->natts; i++)
    {
        Form_pg_attribute current_att = TupleDescAttr(tupleDesc, i);

        if (current_att->attisdropped)
            continue;

        if (current_att->attlen > 0) /* is fixed-width */
        {
            if (i < attnum - 1)
                fixed_off += current_att->attlen;
            fixed_len_total += current_att->attlen;
        }
        else /* is variable-width */
        {
            if (i < attnum - 1)
                var_att_count++;
        }
    }

    if (att->attlen < 0)
        target_var_index = var_att_count;

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
	int i;

	/*
	 * Use our custom getattr function to extract attributes from the
	 * optimized format. This avoids circular dependencies and ensures
	 * we're using the correct extraction logic for our custom format.
	 */
	for (i = 1; i <= natts; i++)
	{
		slot->tts_values[i - 1] = optimized_getattr_for_slot(slot, i, &slot->tts_isnull[i - 1]);
	}
	
	slot->tts_nvalid = natts;
}

static Datum
optimized_getattr_for_slot(TupleTableSlot *slot, int attnum, bool *isnull)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;
	HeapTuple tuple = hslot->tuple;
	TupleDesc tupleDesc = slot->tts_tupleDescriptor;
	Datum datum;
	Form_pg_attribute att;

	/*
	 * Use our custom optimized_extract_attribute function to read data
	 * from the optimized tuple format. This is the correct approach since
	 * our tuples are stored in optimized format, not standard heap format.
	 */
	datum = optimized_extract_attribute(tuple, attnum, tupleDesc, isnull);

	/* Detoast variable-length columns if needed */
	if (!*isnull)
	{
		att = TupleDescAttr(tupleDesc, attnum - 1);
		if (att->attlen == -1)
		{
			datum = PointerGetDatum(pg_detoast_datum((struct varlena *) DatumGetPointer(datum)));
		}
	}

	return datum;
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
                var_col_count++;
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

    /* Add space for variable offsets array */
    len += MAXALIGN(var_col_count * sizeof(uint32));

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
                if (varlena_size > MaxAllocSize || varlena_size < VARHDRSZ)
                {
                    OPTIMIZED_LOG("ERROR: Invalid varlena size %zu for column %d (%s)", 
                                  varlena_size, i + 1, NameStr(att->attname));
                    elog(ERROR, "Invalid varlena size %zu for column %s (expected %d to %zu)",
                         varlena_size, NameStr(att->attname), VARHDRSZ, MaxAllocSize);
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

            /* Store absolute offset from the start of tuple data */
            var_offsets[var_col_index] = base_offset + MAXALIGN(fixed_data_len) + var_pos;

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
	/* Use the heap tuple slot initialization */
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;
	hslot->tuple = NULL;
	hslot->off = 0;
}

static void optimized_slot_release(TupleTableSlot *slot)
{
	/* No special cleanup needed for heap compatible slots */
}

static void optimized_slot_clear(TupleTableSlot *slot)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;
	
	if (TTS_SHOULDFREE(slot))
	{
		heap_freetuple(hslot->tuple);
		slot->tts_flags &= ~TTS_FLAG_SHOULDFREE;
	}
	
	slot->tts_nvalid = 0;
	slot->tts_flags |= TTS_FLAG_EMPTY;
	ItemPointerSetInvalid(&slot->tts_tid);
	hslot->off = 0;
	hslot->tuple = NULL;
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
			values[i] = optimized_extract_attribute(hslot->tuple, i + 1, tupdesc, &isnull[i]);
			
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
			values[i] = optimized_extract_attribute(hslot->tuple, i + 1, tupdesc, &isnull[i]);
			
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
		values[i] = optimized_extract_attribute(hslot->tuple, i + 1, tupdesc, &isnull[i]);
		
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
	.base_slot_size = sizeof(HeapTupleTableSlot),
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
	return &TTSOpsHeapTuple;
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