#include "postgres.h"
#include "access/tableam.h"
#include "access/hio.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tupmacs.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/array.h"
#include "fmgr.h"
#include "access/htup.h"
#include "access/sysattr.h"
#include "access/tupdesc.h"
#include "utils/builtins.h"
#include "utils/relcache.h"
#include "access/heapam.h"
#include "access/tableam.h"
#include "access/tupdesc.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "optimized_row_format.h"

PG_MODULE_MAGIC;

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
 *    - t_hoff points to the start of the null bitmap
 *
 * 2. Null Bitmap (BITMAPLEN(natts) bytes)
 *    - One bit per column indicating NULL status
 *    - 1 = not null, 0 = null
 *    - Always present (HEAP_HASNULL is always set)
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
 *    - Array of offsets pointing to variable-length column data
 *    - Each offset is relative to the start of variable-length data section
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
 *    - Offsets array provides quick access to each column's data
 *
 * Alignment:
 * ---------
 * - Header is MAXALIGN'd
 * - Null bitmap is MAXALIGN'd
 * - Variable offsets array is MAXALIGN'd
 * - Fixed-length data section is MAXALIGN'd
 * - Variable-length data section is MAXALIGN'd
 *
 * Benefits:
 * --------
 * 1. Efficient access to fixed-length columns (no offset calculations needed)
 * 2. Quick NULL checks using bitmap
 * 3. Fast access to variable-length columns through offset array
 * 4. Better cache utilization due to data locality
 * 5. Reduced overhead compared to standard heap format
 *
 * Null Bitmap Access:
 * -----------------
 * When accessing null values in a tuple:
 * 1. Use the original column number (attnum) to check the null bitmap
 * 2. Use the physical position (from get_physical_position()) to access the data
 * 3. The translation between original and physical positions is handled by
 *    get_physical_position() and get_column_offset()
 *
 * Example:
 * - Table: CREATE TABLE t (a int, b text, c int, d text);
 * - Insert: INSERT INTO t VALUES (1, NULL, 2, 'hello');
 * - Null bitmap: [1,0,1,1] (1=not null, 0=null)
 * - Physical layout: [1,2,NULL,'hello']
 */


/* Forward declarations */
static TableAmRoutine optimized_tableam;

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
static void optimized_tuple_insert(Relation relation, TupleTableSlot *slot,
                                 CommandId cid, int options, struct BulkInsertStateData *bistate);
static void optimized_tuple_insert_speculative(Relation relation, TupleTableSlot *slot,
                                             CommandId cid, int options, struct BulkInsertStateData *bistate,
                                             uint32 specToken);
static void optimized_tuple_complete_speculative(Relation relation, TupleTableSlot *slot,
                                               uint32 specToken, bool succeeded);
static Size optimized_parallelscan_estimate(Relation rel);
static bool optimized_relation_needs_toast_table(Relation rel);

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

		optimized_tableam_initialized = true;
	}

    PG_RETURN_POINTER(&optimized_tableam);
}

/* Scan implementation */
static TableScanDesc
optimized_scan_begin(Relation rel, Snapshot snapshot,
                    int nkeys, struct ScanKeyData *key,
                    ParallelTableScanDesc pscan, uint32 flags)
{
    /* For now, delegate to heap AM since scan is not fully implemented */
    const TableAmRoutine *heap_am = get_heap_am_routine();
    return heap_am->scan_begin(rel, snapshot, nkeys, key, pscan, flags);
}

static void
optimized_scan_end(TableScanDesc scan)
{
    /* For now, delegate to heap AM since scan is not fully implemented */
    const TableAmRoutine *heap_am = get_heap_am_routine();
    heap_am->scan_end(scan);
}

static void
optimized_scan_rescan(TableScanDesc scan, ScanKey key, bool set_params,
                     bool allow_strat, bool allow_sync, bool allow_pagemode)
{
    /* For now, delegate to heap AM since scan is not fully implemented */
    const TableAmRoutine *heap_am = get_heap_am_routine();
    heap_am->scan_rescan(scan, key, set_params, allow_strat, allow_sync, allow_pagemode);
}

static bool
optimized_scan_getnextslot(TableScanDesc scan, ScanDirection direction,
                          TupleTableSlot *slot)
{
    HeapTuple tuple;
    bool found;
    bool shouldFree;

    /* First, get the next tuple using heap AM's scanning logic */
    const TableAmRoutine *heap_am = get_heap_am_routine();
    found = heap_am->scan_getnextslot(scan, direction, slot);

    if (!found)
        return false;

    /* Get the tuple from the slot */
    tuple = ExecFetchSlotHeapTuple(slot, false, &shouldFree);
    if (tuple == NULL)
        return false;

    /* Now we need to extract attributes using our custom format */
    TupleDesc tupdesc = slot->tts_tupleDescriptor;
    int natts = tupdesc->natts;

    /* Clear the slot to prepare for our custom values */
    ExecClearTuple(slot);

    /* Extract all attributes using our optimized format */
    for (int i = 0; i < natts; i++)
    {
        Datum value;
        bool isnull;

        /* Use our custom getattr function to extract the value */
        value = optimized_getattr(tuple, i + 1, tupdesc, &isnull);

        /* Store the value directly in the slot */
        slot->tts_values[i] = value;
        slot->tts_isnull[i] = isnull;
    }

    /* Mark the slot as having valid data */
    slot->tts_nvalid = natts;
    slot->tts_flags &= ~TTS_FLAG_EMPTY;

    /* If we need to free the tuple, do it now */
    if (shouldFree && tuple)
        heap_freetuple(tuple);

    return true;
}

static Size
optimized_parallelscan_estimate(Relation rel)
{
	/* For now, delegate to heap AM since scan is not fully implemented */
	const TableAmRoutine *heap_am = get_heap_am_routine();
	return heap_am->parallelscan_estimate(rel);
}

/*
 * optimized_extract_attribute
 *      Extract a single attribute from an optimized tuple format.
 *      This function understands our custom layout and extracts data directly.
 */
static Datum
optimized_extract_attribute(HeapTuple tuple, int attnum, TupleDesc tupleDesc, bool *isnull)
{
    OptimizedTupleHeader header = (OptimizedTupleHeader) tuple->t_data;
    Form_pg_attribute att = TupleDescAttr(tupleDesc, attnum - 1);
    char *null_bitmap;
    uint32 *var_col_count_ptr;
    uint32 var_col_count;
    uint32 *var_offsets;
    char *fixed_data;
    char *var_data;
    int fixed_pos = 0;
    int i;

    elog(DEBUG1, "optimized_extract_attribute: attnum=%d, attname=%s, attlen=%d",
         attnum, NameStr(att->attname), att->attlen);

    /* Set up pointers to data sections */
    null_bitmap = (char *) header + header->t_hoff;
    var_col_count_ptr = (uint32 *) (null_bitmap + MAXALIGN(BITMAPLEN(tupleDesc->natts)));
    var_col_count = *var_col_count_ptr;
    var_offsets = (uint32 *) ((char *)var_col_count_ptr + MAXALIGN(sizeof(uint32)));

    elog(DEBUG1, "optimized_extract_attribute: var_col_count=%u, natts=%d",
         var_col_count, tupleDesc->natts);

    /* Calculate fixed data length to find where variable data starts */
    // todo : might need change design as this is not efficient,
    // we should store this info somewhere else
    Size fixed_data_len = 0;
    for (i = 0; i < tupleDesc->natts; i++)
    {
        Form_pg_attribute curr_att = TupleDescAttr(tupleDesc, i);
        if (!curr_att->attisdropped && curr_att->attlen > 0)
            fixed_data_len += curr_att->attlen;
    }

    fixed_data = (char *) (var_offsets) + MAXALIGN(var_col_count * sizeof(uint32));
    var_data = fixed_data + MAXALIGN(fixed_data_len);

    elog(DEBUG1, "optimized_extract_attribute: fixed_data_len=%zu, fixed_data=%p, var_data=%p",
         fixed_data_len, fixed_data, var_data);

    *isnull = false;

    /* Handle fixed-length columns */
    if (att->attlen > 0)
    {
        /* Calculate offset within fixed data section */
        for (i = 0; i < attnum - 1; i++)
        {
            Form_pg_attribute curr_att = TupleDescAttr(tupleDesc, i);
            if (!curr_att->attisdropped && curr_att->attlen > 0)
                fixed_pos += curr_att->attlen;
        }

        elog(DEBUG1, "optimized_extract_attribute: fixed column, fixed_pos=%d", fixed_pos);

        /* Extract the data directly based on attribute type */
        char *data_ptr = fixed_data + fixed_pos;

        if (att->attbyval)
        {
            /* Pass-by-value: extract the value based on length */
            switch (att->attlen)
            {
                case sizeof(char):
                    {
                        char val = *((char *) data_ptr);
                        elog(DEBUG1, "optimized_extract_attribute: char value=%d", val);
                        return CharGetDatum(val);
                    }
                case sizeof(int16):
                    {
                        int16 val = *((int16 *) data_ptr);
                        elog(DEBUG1, "optimized_extract_attribute: int16 value=%d", val);
                        return Int16GetDatum(val);
                    }
                case sizeof(int32):
                    {
                        int32 val = *((int32 *) data_ptr);
                        elog(DEBUG1, "optimized_extract_attribute: int32 value=%d", val);
                        return Int32GetDatum(val);
                    }
#if SIZEOF_DATUM == 8
                case sizeof(Datum):
                    {
                        Datum val = *((Datum *) data_ptr);
                        elog(DEBUG1, "optimized_extract_attribute: Datum value=%ld", val);
                        return val;
                    }
#endif
                default:
                    elog(ERROR, "unsupported byval length: %d", att->attlen);
                    return (Datum) 0;
            }
        }
        else
        {
            /* Pass-by-reference: return pointer to the data */
            elog(DEBUG1, "optimized_extract_attribute: fixed pass-by-ref, data_ptr=%p", data_ptr);
            return PointerGetDatum(data_ptr);
        }
    }
    /* Handle variable-length columns */
    else
    {
        /* Find which variable column this is */
        int var_index = 0;
        for (i = 0; i < attnum - 1; i++)
        {
            Form_pg_attribute curr_att = TupleDescAttr(tupleDesc, i);
            if (!curr_att->attisdropped && curr_att->attlen <= 0)
                var_index++;
        }

        elog(DEBUG1, "optimized_extract_attribute: variable column, var_index=%d", var_index);

        /* Get the data from the variable section using the offset */
        if (var_index < var_col_count)
        {
            uint32 offset = var_offsets[var_index];
            char *var_data_ptr = var_data + offset;
            elog(DEBUG1, "optimized_extract_attribute: var offset=%u, var_data_ptr=%p", offset, var_data_ptr);
            /* Variable-length data is always pass-by-reference */
            return PointerGetDatum(var_data_ptr);
        }
        else
        {
            elog(DEBUG1, "optimized_extract_attribute: var_index >= var_col_count, returning NULL");
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
    if (attnum > 0)
    {
        /* User attribute */
        if (attnum > tupleDesc->natts)
            return getmissingattr(tupleDesc, attnum, isnull);

        /* Check for null value using the null bitmap */
        if (HeapTupleHasNulls(tuple))
        {
            if (att_isnull(attnum - 1, tuple->t_data->t_bits))
            {
                *isnull = true;
                return (Datum) NULL;
            }
        }

        /* Extract the attribute using our custom format */
        return optimized_extract_attribute(tuple, attnum, tupleDesc, isnull);
    }
    else
    {
        /* System attribute */
        return heap_getsysattr(tuple, attnum, tupleDesc, isnull);
    }
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
    char *null_bitmap;
    ItemPointerData InvalidItemPointer;
    int var_col_index = 0;  /* Track which variable column we're processing */

    /* First pass: Count columns and calculate lengths */
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute att = TupleDescAttr(tupdesc, i);
        if (!att->attisdropped)
        {
            if (att->attlen > 0)
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

    /* Calculate total length needed */
    len = SizeofOptimizedTupleHeader;
    len = MAXALIGN(len);  /* Align header */

    /* Add space for null bitmap */
    len += BITMAPLEN(tupdesc->natts);
    len = MAXALIGN(len);  /* Align null bitmap */

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
        Datum value;
        bool isnull;

        if (att->attisdropped || att->attlen > 0)
            continue;

        value = slot_getattr(slot, i + 1, &isnull);
        if (!isnull)
        {
            if (att->attlen == -1)  /* varlena */
            {
                var_data_len += VARSIZE_ANY(DatumGetPointer(value));
            }
            else  /* cstring */
            {
                var_data_len += strlen(DatumGetCString(value)) + 1;
            }
        }
    }

    /* Add space for variable-length columns */
    len += MAXALIGN(var_data_len);

	ItemPointerSetInvalid(&InvalidItemPointer);

    /* Allocate the tuple */
    tuple = (HeapTuple) palloc0(HEAPTUPLESIZE + len);
    tuple->t_len = len;
    tuple->t_self = InvalidItemPointer;
    tuple->t_tableOid = RelationGetRelid(relation);
    tuple->t_data = (HeapTupleHeader) ((char *) tuple + HEAPTUPLESIZE);

    /* Initialize the header */
    header = (OptimizedTupleHeader) tuple->t_data;
    header->t_len = len;
    header->t_infomask = HEAP_HASNULL;  /* We always have null bitmap */
    header->t_infomask2 = 0;  /* Initialize to 0 first */
    HeapTupleHeaderSetNatts(header, tupdesc->natts);  /* Set natts properly */
    header->t_hoff = SizeofOptimizedTupleHeader;

    /* Set up pointers to data sections */
    null_bitmap = (char *) header + header->t_hoff;
    uint32 *var_col_count_ptr = (uint32 *) (null_bitmap + MAXALIGN(BITMAPLEN(tupdesc->natts)));
    var_offsets = (uint32 *) ((char *)var_col_count_ptr + MAXALIGN(sizeof(uint32)));
    fixed_data = (char *) (var_offsets) + MAXALIGN(var_col_count * sizeof(uint32));
    var_data = fixed_data + MAXALIGN(fixed_data_len);

    /* Initialize null bitmap to all zeros */
    memset(null_bitmap, 0, BITMAPLEN(tupdesc->natts));

    /* Store the variable column count */
    *var_col_count_ptr = var_col_count;

    /* Second pass: Copy data into reorganized layout */
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute att = TupleDescAttr(tupdesc, i);
        Datum value;
        bool isnull;

        if (att->attisdropped)
            continue;

        value = slot_getattr(slot, i + 1, &isnull);

        if (isnull)
        {
            /* Set the null bit for this column (0 = null) */
            header->t_infomask |= HEAP_HASNULL;
            /* Bit is already 0 from memset, so nothing to do */
        }
        else
        {
            /* Set the bit (1 = not null) */
            null_bitmap[i >> 3] |= (1 << (i & 0x07));
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
            /* Variable-length column - store offset and copy data */
            Size varlen;
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
            var_offsets[var_col_index] = var_pos;
            var_pos += varlen;
            var_col_index++;
        }
    }

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

    /* Mark the buffer dirty */
    MarkBufferDirty(buffer);

    /* Release the buffer */
    UnlockReleaseBuffer(buffer);

    /* Free the tuple */
    pfree(tuple);
}

static void
optimized_tuple_insert_speculative(Relation relation, TupleTableSlot *slot,
                                 CommandId cid, int options, struct BulkInsertStateData *bistate,
                                 uint32 specToken)
{
    /* TODO: Implement speculative insert */
}

static void
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