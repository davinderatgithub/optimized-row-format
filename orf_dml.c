#include "postgres.h"
#include "access/heapam.h"
#include "access/hio.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"
#include "miscadmin.h"
#include "utils/memutils.h"

#include "optimized_row_format.h"
#include "orf_dml.h"
#include "orf_utils.h" /* For choose_offset_encoding */

/*
 * Custom logging for optimized row format extension
 * DISABLED for testing - uncomment to enable debugging
 */
// #define OPTIMIZED_LOG(fmt, ...) \
//     elog(NOTICE, "OPTIMIZED_DEBUG: " fmt, ##__VA_ARGS__)
#define OPTIMIZED_LOG(fmt, ...) do { } while (0)

/*
 * optimized_tuple_delete - delete a tuple from an optimized table
 *      This function implements DELETE operations for the optimized row format
 *      by marking the tuple as deleted using PostgreSQL's MVCC mechanism.
 */
TM_Result
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
TM_Result
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
    Buffer      newbuf;
    OffsetNumber newoffnum;
    ItemPointerData newtid;
	Size estimated_tuple_size;
	TupleTableSlot *temp_slot;

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
    estimated_tuple_size = oldtup.t_len * 2; // Conservative estimate
    
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
    temp_slot = slot; // Use the same slot
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
void
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
	HeapTuple heap_tuple;
	OffsetEncodingType offset_encoding;
	Size offset_size;
	Size var_offsets_size;
	Size heap_tuple_size;
	Size optimized_tuple_size;
	Size overhead;
	float overhead_percent;
	Size header_size;
	Size null_bitmap_size;
	Size var_count_size;
	Size var_offsets_analysis_size;
	Size fixed_data_size;
	Size var_data_size;
	Size unaligned_total;
	Size alignment_waste;
	float alignment_waste_percent;
	uint32 absolute_offset;


    OPTIMIZED_LOG("Starting optimized tuple insert for relation %s",
                  RelationGetRelationName(relation));

    /* Pre-allocate arrays to check for nulls */
    isnull_array = (bool *) palloc0(tupdesc->natts * sizeof(bool));
    values_array = (Datum *) palloc(tupdesc->natts * sizeof(Datum));

    /* Get the heap tuple from slot for direct extraction */
    heap_tuple = ExecFetchSlotHeapTuple(slot, false, NULL);
    
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
    offset_encoding = choose_offset_encoding(len + var_data_len, var_col_count);
    offset_size = (offset_encoding == OFFSET_ENCODING_16BIT) ? sizeof(uint16) : sizeof(uint32);
    
    /* Add space for variable offsets array with optimized encoding */
    var_offsets_size = MAXALIGN(var_col_count * offset_size);
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
        heap_tuple_size = heap_tuple->t_len;
        optimized_tuple_size = len;
        overhead = optimized_tuple_size - heap_tuple_size;
        overhead_percent = ((float)overhead / heap_tuple_size) * 100.0f;
        
        /* Calculate component sizes for detailed analysis */
        header_size = SizeofOptimizedTupleHeader;
        null_bitmap_size = hasnull ? MAXALIGN(BITMAPLEN(tupdesc->natts)) : 0;
        var_count_size = MAXALIGN(sizeof(uint32));
        var_offsets_analysis_size = var_offsets_size;  /* Use calculated size with encoding */
        fixed_data_size = MAXALIGN(fixed_data_len);
        var_data_size = MAXALIGN(var_data_len);
        
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
        unaligned_total = header_size + (hasnull ? BITMAPLEN(tupdesc->natts) : 0) + 
                              sizeof(uint32) + (var_col_count * sizeof(uint32)) + 
                              fixed_data_len + var_data_len;
        alignment_waste = optimized_tuple_size - unaligned_total;
        alignment_waste_percent = ((float)alignment_waste / heap_tuple_size) * 100.0f;
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
			absolute_offset = base_offset + MAXALIGN(fixed_data_len) + var_pos;

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
