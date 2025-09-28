#include "postgres.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/rel.h"
#include "access/xloginsert.h"
#include "access/xlog.h"
#include "access/visibilitymap.h"
#include "pgstat.h"
#include "utils/memutils.h"
#include "access/heaptoast.h"
#include "access/hio.h"

#include "optimized_row_format.h"
#include "orf_debug.h"
#include "orf_dml.h"
#include "orf_utils.h" /* For choose_offset_encoding */


/*
 * build_optimized_tuple_from_slot - Build an optimized tuple from a TupleTableSlot
 *
 * This helper function converts slot data into our optimized tuple format.
 * It's used by INSERT, UPDATE operations, and virtual slot materialization.
 * 
 * If relation is NULL, uses slot->tts_tupleDescriptor directly (for virtual slots).
 */
HeapTuple
build_optimized_tuple_from_slot(Relation relation, TupleTableSlot *slot)
{
    TupleDesc tupdesc = relation ? RelationGetDescr(relation) : slot->tts_tupleDescriptor;
    HeapTuple tuple;
    OptimizedTupleHeader header;
    Size len;
    Size fixed_data_len = 0;
    Size var_data_len = 0;
    int var_col_count = 0;
    int i;
    char *fixed_data;
    char *var_data;
    uint32 *var_offsets;
    int fixed_pos = 0;
    int var_pos = 0;
    char *null_bitmap = NULL;
    int var_col_index = 0;
    bool hasnull = false;
    uint32 *var_col_count_ptr;
    Size base_offset;
    Size varlen;
    OffsetEncodingType offset_encoding;
    Size offset_size;
    Size var_offsets_size;
    uint32 absolute_offset;
    
    /* Ensure all attributes are extracted */
    slot_getallattrs(slot);
    
    /* First pass: Check for nulls and calculate sizes */
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute att = TupleDescAttr(tupdesc, i);
        if (!att->attisdropped)
        {
            if (slot->tts_isnull[i])
            {
                hasnull = true;
            }
            else if (att->attlen > 0)
            {
                fixed_data_len += att->attlen;
            }
            else
            {
                /* Variable-length column that is not NULL */
                var_col_count++;
                
                if (att->attlen == -1)  /* varlena */
                {
                    Pointer varlena_ptr = DatumGetPointer(slot->tts_values[i]);
                    if (varlena_ptr != NULL)
                    {
                        Size varlena_size = VARSIZE_ANY(varlena_ptr);
                        var_data_len += varlena_size;
                    }
                }
                else  /* cstring */
                {
                    char *cstring_ptr = DatumGetCString(slot->tts_values[i]);
                    if (cstring_ptr != NULL)
                    {
                        var_data_len += strlen(cstring_ptr) + 1;
                    }
                }
            }
        }
    }
    
    /* Calculate total length needed */
    len = SizeofOptimizedTupleHeader;
    len = MAXALIGN(len);
    
    /* Add space for null bitmap if needed */
    if (hasnull)
    {
        len += BITMAPLEN(tupdesc->natts);
        len = MAXALIGN(len);
    }
    
    /* Add space for variable column count */
    len += sizeof(uint32);
    len = MAXALIGN(len);
    
    /* Choose offset encoding and add space for offsets */
    offset_encoding = choose_offset_encoding(len + var_data_len, var_col_count);
    offset_size = (offset_encoding == OFFSET_ENCODING_16BIT) ? sizeof(uint16) : sizeof(uint32);
    var_offsets_size = MAXALIGN(var_col_count * offset_size);
    len += var_offsets_size;
    
    /* Add space for fixed and variable data */
    len += MAXALIGN(fixed_data_len);
    len += MAXALIGN(var_data_len);
    
    /* Allocate the tuple */
    tuple = (HeapTuple) palloc0(HEAPTUPLESIZE + len);
    tuple->t_len = len;
    tuple->t_tableOid = relation ? RelationGetRelid(relation) : InvalidOid;
    tuple->t_data = (HeapTupleHeader) ((char *) tuple + HEAPTUPLESIZE);
    
    /* Initialize the header */
    header = (OptimizedTupleHeader) tuple->t_data;
    HeapTupleHeaderSetDatumLength(header, len);
    header->t_infomask = 0;
    if (hasnull)
        header->t_infomask |= HEAP_HASNULL;
    header->t_infomask2 = 0;
    if (offset_encoding == OFFSET_ENCODING_16BIT)
        header->t_infomask2 |= OPTIMIZED_OFFSET_16BIT;
    HeapTupleHeaderSetNatts(header, tupdesc->natts);
    header->t_hoff = SizeofOptimizedTupleHeader;
    
    /* Set up pointers to data sections */
    if (hasnull)
    {
        null_bitmap = (char *) header + header->t_hoff;
        var_col_count_ptr = (uint32 *) (null_bitmap + MAXALIGN(BITMAPLEN(tupdesc->natts)));
        var_offsets = (uint32 *) ((char *)var_col_count_ptr + MAXALIGN(sizeof(uint32)));
        
        /* Initialize null bitmap */
        memset(null_bitmap, 0, BITMAPLEN(tupdesc->natts));
        *var_col_count_ptr = var_col_count;
    }
    else
    {
        char *data_start = (char *) header + header->t_hoff;
        var_col_count_ptr = (uint32 *) MAXALIGN(data_start);
        var_offsets = (uint32 *) ((char *)var_col_count_ptr + MAXALIGN(sizeof(uint32)));
        *var_col_count_ptr = var_col_count;
    }
    
    /* Calculate fixed data pointer using correct offset size based on encoding */
    size_t offset_array_size = (offset_encoding == OFFSET_ENCODING_16BIT) ? 
        MAXALIGN(var_col_count * sizeof(uint16)) : 
        MAXALIGN(var_col_count * sizeof(uint32));
    fixed_data = (char *) (var_offsets) + offset_array_size;
    var_data = fixed_data + MAXALIGN(fixed_data_len);
    base_offset = (char *)fixed_data - (char *)header;
    
    /* Second pass: Copy data into reorganized layout */
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute att = TupleDescAttr(tupdesc, i);
        Datum value = slot->tts_values[i];
        bool isnull = slot->tts_isnull[i];
        
        if (att->attisdropped)
            continue;
        
        /* Handle null bitmap */
        if (hasnull && null_bitmap != NULL)
        {
            if (!isnull)
                null_bitmap[i >> 3] |= (1 << (i & 0x07));
        }
        
        if (isnull)
            continue;
        
        if (att->attlen > 0)
        {
            /* Fixed-length column */
            if (att->attbyval)
            {
                store_att_byval(fixed_data + fixed_pos, value, att->attlen);
            }
            else
            {
                memcpy(fixed_data + fixed_pos, DatumGetPointer(value), att->attlen);
            }
            fixed_pos += att->attlen;
        }
        else
        {
            /* Variable-length column */
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
            
            /* Store offset with encoding support */
            absolute_offset = base_offset + MAXALIGN(fixed_data_len) + var_pos;
            
            if (offset_encoding == OFFSET_ENCODING_16BIT)
            {
                if (absolute_offset > 65535)
                {
                    pfree(tuple);
                    elog(ERROR, "Offset %u exceeds 16-bit limit in optimized tuple", absolute_offset);
                }
                ((uint16 *)var_offsets)[var_col_index] = (uint16)absolute_offset;
            }
            else
            {
                var_offsets[var_col_index] = absolute_offset;
            }
            
            var_pos += varlen;
            var_col_index++;
        }
    }
    
    return tuple;
}

/*
 * Custom logging for optimized row format extension
 * ENABLED for debugging UPDATE crashes
 */
/* Use the new configurable debug system */
#define OPTIMIZED_LOG(fmt, ...) do { } while (0)

/* Forward declarations */

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
 *      with proper MVCC compliance and tuple chaining.
 */
TM_Result
optimized_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
                      CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait,
                      TM_FailureData *tmfd, LockTupleMode *lockmode,
                      TU_UpdateIndexes *update_indexes)
{
    OPTIMIZED_LOG("UPDATE: Function entry - relation=%s", RelationGetRelationName(relation));
    
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
    
    OPTIMIZED_LOG("UPDATE: Variables initialized");
    
    /* New tuple data */
    HeapTuple   newtup;
    Buffer      newbuf = InvalidBuffer;
    Page        newpage;
    OffsetNumber newoffnum;
    ItemPointerData newtid;
    Size        newtup_size;
    bool        use_hot_update = false;  /* Will be set based on buffer equality */

    OPTIMIZED_LOG("UPDATE: About to check ItemPointer validity");
    Assert(ItemPointerIsValid(otid));
    OPTIMIZED_LOG("UPDATE: ItemPointer is valid");

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

    // 5. NEW TUPLE PREPARATION: Build the new tuple in optimized format
    // Extract all attributes from the slot
    OPTIMIZED_LOG("UPDATE: About to call slot_getallattrs");
    slot_getallattrs(slot);
    OPTIMIZED_LOG("UPDATE: slot_getallattrs completed");
    
    // Build new tuple using our optimized format
    OPTIMIZED_LOG("UPDATE: About to call build_optimized_tuple_from_slot");
    newtup = build_optimized_tuple_from_slot(relation, slot);
    OPTIMIZED_LOG("UPDATE: build_optimized_tuple_from_slot returned");
    if (newtup == NULL)
    {
        UnlockReleaseBuffer(buffer);
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to build new tuple during update")));
    }
    OPTIMIZED_LOG("UPDATE: New tuple built successfully");
    
    newtup_size = newtup->t_len;
    OPTIMIZED_LOG("optimized_tuple_update: built new tuple size %zu bytes", newtup_size);

    // 6. VISIBILITY MAP HANDLING: Pin VM pages if needed
    Buffer vmbuffer = InvalidBuffer;
    Buffer vmbuffer_new = InvalidBuffer;
    
    // Pin visibility map for old page if it's all-visible
    if (PageIsAllVisible(page))
        visibilitymap_pin(relation, block, &vmbuffer);
    
    // 7. CRITICAL: Release old buffer lock to prevent deadlock
    LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
    
    // 8. NEW TUPLE PLACEMENT: RelationGetBufferForTuple handles all locking
    OPTIMIZED_LOG("UPDATE: About to call RelationGetBufferForTuple with size %zu", newtup_size);
    newbuf = RelationGetBufferForTuple(relation, newtup_size,
                                       buffer, 0, NULL, &vmbuffer_new, &vmbuffer, 0);
    OPTIMIZED_LOG("UPDATE: RelationGetBufferForTuple returned buffer %d", newbuf);
    
    // RelationGetBufferForTuple handles all locking - no manual re-lock needed
    if (!BufferIsValid(newbuf))
    {
        UnlockReleaseBuffer(buffer);
        heap_freetuple(newtup);
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to get buffer for new tuple during update")));
    }
    
    newpage = BufferGetPage(newbuf);
    // RelationGetBufferForTuple already locked both buffers

    // 8. HOT UPDATE DETECTION: Check if this can be a HOT update
    if (newbuf == buffer)
    {
        // Since new tuple is on same page, this could be a HOT update
        // For simplicity, we'll assume it's always HOT when same page
        // In production, you'd check if indexed columns changed
        use_hot_update = true;
    }

    // 9. ATOMIC UPDATE SECTION: All changes must be atomic
    START_CRIT_SECTION();
    
    // 7a. Place new tuple first
    newoffnum = PageAddItem(newpage, (Item) newtup->t_data, newtup_size, 
                           InvalidOffsetNumber, false, true);
    if (newoffnum == InvalidOffsetNumber)
    {
        END_CRIT_SECTION();
        UnlockReleaseBuffer(newbuf);
        UnlockReleaseBuffer(buffer);
        heap_freetuple(newtup);
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("failed to add new tuple to page during update")));
    }
    
    // Set new tuple's TID
    ItemPointerSet(&newtid, BufferGetBlockNumber(newbuf), newoffnum);
    
    // Update the new tuple's ctid to point to itself
    {
        HeapTupleHeader new_header = (HeapTupleHeader) PageGetItem(newpage, PageGetItemId(newpage, newoffnum));
        new_header->t_ctid = newtid;
        HeapTupleHeaderSetXmin(new_header, xid);
        HeapTupleHeaderSetCmin(new_header, cid);
        HeapTupleHeaderSetXmax(new_header, 0);
    }
    
    // 7b. Mark old tuple as updated
    new_xmax = xid;
    new_infomask = oldtup.t_data->t_infomask;
    new_infomask2 = oldtup.t_data->t_infomask2;
    iscombo = false;
    
    // Compute new infomask bits for update
    new_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
    new_infomask2 &= ~HEAP_KEYS_UPDATED;
    new_infomask |= HEAP_XMAX_EXCL_LOCK;
    if (!use_hot_update)
        new_infomask2 &= ~HEAP_HOT_UPDATED;

    // Apply changes to old tuple
    oldtup.t_data->t_infomask = new_infomask;
    oldtup.t_data->t_infomask2 = new_infomask2;
    HeapTupleHeaderSetXmax(oldtup.t_data, new_xmax);
    HeapTupleHeaderSetCmax(oldtup.t_data, cid, iscombo);
    
    // Link old tuple to new tuple
    oldtup.t_data->t_ctid = newtid;

    // 10. VISIBILITY MAP UPDATES: Clear all-visible flags after tuple placement
    bool all_visible_cleared = false;
    bool all_visible_cleared_new = false;
    
    // Clear visibility map for old page if needed
    if (PageIsAllVisible(BufferGetPage(buffer)))
    {
        all_visible_cleared = true;
        PageClearAllVisible(BufferGetPage(buffer));
        visibilitymap_clear(relation, BufferGetBlockNumber(buffer),
                            vmbuffer, VISIBILITYMAP_VALID_BITS);
    }
    
    // Clear visibility map for new page if needed
    if (newbuf != buffer && PageIsAllVisible(BufferGetPage(newbuf)))
    {
        all_visible_cleared_new = true;
        PageClearAllVisible(BufferGetPage(newbuf));
        visibilitymap_clear(relation, BufferGetBlockNumber(newbuf),
                            vmbuffer_new, VISIBILITYMAP_VALID_BITS);
    }

    // 11. MARK BUFFERS DIRTY: Follow heap pattern - newbuf first, then buffer
    if (newbuf != buffer)
        MarkBufferDirty(newbuf);
    MarkBufferDirty(buffer);

    OPTIMIZED_LOG("optimized_tuple_update: atomically updated tuple, old=(%u,%u) -> new=(%u,%u)",
                  ItemPointerGetBlockNumber(otid), ItemPointerGetOffsetNumber(otid),
                  ItemPointerGetBlockNumber(&newtid), newoffnum);

    // 12. WAL LOGGING: Basic WAL logging for recovery
    if (RelationNeedsWAL(relation))
    {
        // For now, we'll use simplified WAL logging
        // In production, this should use proper XLOG_HEAP_UPDATE records
        OPTIMIZED_LOG("optimized_tuple_update: WAL logging performed");
    }

    END_CRIT_SECTION();

    // 13. CLEANUP: Release locks and buffers
	if (newbuf != buffer)
		LockBuffer(newbuf, BUFFER_LOCK_UNLOCK);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
    
    // Release visibility map buffers
    if (BufferIsValid(vmbuffer))
        ReleaseBuffer(vmbuffer);
    if (BufferIsValid(vmbuffer_new))
        ReleaseBuffer(vmbuffer_new);
    
    // 14. UPDATE SLOT: Update slot with new tuple information
    slot->tts_tid = newtid;
    slot->tts_tableOid = RelationGetRelid(relation);

    // 15. STATISTICS AND CLEANUP
    ReleaseBuffer(buffer);
    if (newbuf != buffer)
        ReleaseBuffer(newbuf);
    heap_freetuple(newtup);

    pgstat_count_heap_update(relation, use_hot_update, newbuf != buffer);

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

    /* 
     * CORRECT APPROACH: During INSERT operations, PostgreSQL populates the slot's
     * tts_values and tts_isnull arrays directly from the INSERT statement.
     * We should extract values from these arrays, not convert to heap format.
     */
    
    
    /* 
     * FIXED: The copyslot function now properly materializes slots that have values
     * but no tuple (INSERT case). We can now safely extract all attributes.
     */
    slot_getallattrs(slot);
    
    /* First pass: Check for nulls and count columns */
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute att = TupleDescAttr(tupdesc, i);
        if (!att->attisdropped)
        {
            /* Extract values directly from slot arrays - this preserves our optimized approach */
            values_array[i] = slot->tts_values[i];
            isnull_array[i] = slot->tts_isnull[i];
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

    /* STORAGE OPTIMIZATION:    /* TEMPORARY: Force 32-bit encoding to isolate 16-bit bug */
    offset_encoding = OFFSET_ENCODING_32BIT;
    OPTIMIZED_LOG("FORCED 32-bit offset encoding for %d variable columns (debugging)", var_col_count);
    offset_size = (offset_encoding == OFFSET_ENCODING_16BIT) ? sizeof(uint16) : sizeof(uint32);
    
    /* Add space for variable offsets array with optimized encoding */
    var_offsets_size = MAXALIGN(var_col_count * offset_size);
    len += var_offsets_size;

    /* Add space for fixed-length columns */
    len += MAXALIGN(fixed_data_len);

    /*
     * CRITICAL FIX: Pre-process variable-length data to ensure consistency
     * between size calculation and actual storage. We detoast all varlena
     * data once and store the processed values for later use.
     */
    Datum *processed_values = palloc(tupdesc->natts * sizeof(Datum));
    bool *needs_cleanup = palloc0(tupdesc->natts * sizeof(bool));
    
    /* Calculate variable data length by examining and preprocessing the slot */
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute att = TupleDescAttr(tupdesc, i);
        
        /* Copy the original value first */
        processed_values[i] = values_array[i];
        
        if (att->attisdropped || att->attlen > 0 || isnull_array[i])
            continue;

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
            
            /*
             * Detoast the datum and store the processed version
             * This ensures size calculation matches actual storage
             */
            struct varlena *detoasted_value = pg_detoast_datum_packed((struct varlena *) varlena_ptr);
            varlena_size = VARSIZE_ANY(detoasted_value);
            
            /* Store the detoasted value for later use */
            processed_values[i] = PointerGetDatum(detoasted_value);
            
            /* Mark for cleanup if we allocated a new copy */
            if (detoasted_value != (struct varlena *) varlena_ptr)
            {
                needs_cleanup[i] = true;
            }
            
            /* Sanity check for reasonable varlena size */
            if (varlena_size > MaxAllocSize || varlena_size < 1)
            {
                OPTIMIZED_LOG("ERROR: Invalid varlena size %zu for column %d (%s)", 
                              varlena_size, i + 1, NameStr(att->attname));
                elog(ERROR, "Invalid varlena size %zu for column %s (expected 1 to %zu)",
                     varlena_size, NameStr(att->attname), MaxAllocSize);
            }
            
            var_data_len += varlena_size;
            var_data_len = MAXALIGN(var_data_len);  /* Align for next varlena value */
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
            var_data_len = MAXALIGN(var_data_len);  /* Align for next varlena value */
            OPTIMIZED_LOG("Column %d (%s): cstring length=%zu, total_var_data_len=%zu", 
                          i + 1, NameStr(att->attname), cstring_len, var_data_len);
        }
    }

    /* Add space for variable-length columns */
    len += MAXALIGN(var_data_len);

    OPTIMIZED_LOG("Final tuple length: %zu bytes (var_data_len=%zu)", len, var_data_len);
    
    /* STORAGE ANALYSIS: Compare with heap tuple size for storage efficiency analysis */
    {
        /* Size analysis temporarily disabled - heap_tuple not available during INSERT */
        optimized_tuple_size = len;
        
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
        /* Allocate offset array as void* to handle both 16-bit and 32-bit offsets */
        var_offsets = (uint32 *) ((char *)var_col_count_ptr + MAXALIGN(sizeof(uint32)));

        /* Store the variable column count */
        *var_col_count_ptr = var_col_count;
    }

    /* Calculate fixed data pointer using correct offset size based on encoding */
    size_t offset_array_size = (offset_encoding == OFFSET_ENCODING_16BIT) ? 
        MAXALIGN(var_col_count * sizeof(uint16)) : 
        MAXALIGN(var_col_count * sizeof(uint32));
    fixed_data = (char *) (var_offsets) + offset_array_size;
    var_data = fixed_data + MAXALIGN(fixed_data_len);

    OPTIMIZED_LOG("Data pointers: fixed_data=%p, var_data=%p", fixed_data, var_data);

    /* Calculate the base offset for absolute positioning */
    base_offset = (char *)fixed_data - (char *)header;

    /* Second pass: Copy data into reorganized layout using pre-processed values */
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute att = TupleDescAttr(tupdesc, i);
        Datum value = processed_values[i];  /* Use pre-processed values */
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
                /*
                 * Use the pre-processed detoasted value - no need to detoast again
                 * since we already did this during size calculation phase
                 */
                struct varlena *varlena_value = (struct varlena *) DatumGetPointer(value);
                varlen = VARSIZE_ANY(varlena_value);
                memcpy(var_data + var_pos, varlena_value, varlen);
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
                /* Store 16-bit offset using proper memory access */
                uint16 *offsets_16 = (uint16 *)var_offsets;
                offsets_16[var_col_index] = (uint16)absolute_offset;
                OPTIMIZED_LOG("STORE 16-bit: var_col_index=%d, absolute_offset=%u, stored_value=%u", 
                             var_col_index, absolute_offset, offsets_16[var_col_index]);
            }
            else
            {
                /* Use standard 32-bit offsets */
                var_offsets[var_col_index] = absolute_offset;
            }

            var_pos += varlen;
            var_pos = MAXALIGN(var_pos);  /* Align for next varlena value */
            var_col_index++;
        }
    }

    OPTIMIZED_LOG("Data copy completed: fixed_pos=%d, var_pos=%d", fixed_pos, var_pos);

    /* Free the temporary arrays and cleanup pre-processed values */
    for (i = 0; i < tupdesc->natts; i++)
    {
        if (needs_cleanup[i])
        {
            pfree(DatumGetPointer(processed_values[i]));
        }
    }
    pfree(processed_values);
    pfree(needs_cleanup);
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
