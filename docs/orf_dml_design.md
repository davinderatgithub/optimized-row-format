# ORF DML Operations Design Document

This document outlines the design for implementing `UPDATE` and `DELETE` operations in the optimized_row_format extension, building upon the analysis provided in `sme_heap_am_analysis.md`.

## Executive Summary

The optimized_row_format extension currently implements scanning and insertion operations but lacks DML (Data Manipulation Language) operations for `UPDATE` and `DELETE`. This design document specifies how to implement these operations while:

1. Maintaining full MVCC (Multi-Version Concurrency Control) compatibility
2. Preserving the optimized storage format benefits
3. Ensuring seamless integration with PostgreSQL's table access method framework
4. Supporting all PostgreSQL features (HOT updates, WAL logging, visibility maps, etc.)

## Current ORF Architecture Review

### Storage Format
The ORF uses an optimized tuple layout:
```
[Tuple Header] -> [Null Bitmap] -> [Var Column Count] -> [Var Offsets Array] -> [Fixed Columns] -> [Variable Columns]
```

Key characteristics:
- Uses standard `HeapTupleHeaderData` for MVCC compatibility
- Fixed-length columns stored first for optimal access
- Variable-length columns accessed via absolute offset array
- Null bitmap maintains original column ordering for compatibility
- Column cache (`OptimizedColumnMapCache`) provides O(1) attribute access

### Current Table AM Integration
```c
// Currently implemented:
optimized_tableam.tuple_insert = optimized_tuple_insert;
optimized_tableam.scan_begin = optimized_scan_begin;
optimized_tableam.scan_getnextslot = optimized_scan_getnextslot;

// Missing (to be implemented):
optimized_tableam.tuple_delete = optimized_tuple_delete;
optimized_tableam.tuple_update = optimized_tuple_update;
```

## Design Principles

### 1. MVCC Compliance
- Never modify tuples in-place (following PostgreSQL's MVCC model)
- Use transaction IDs (`xmin`/`xmax`) for visibility control
- Implement proper snapshot visibility checking
- Support tuple locking mechanisms

### 2. Format Compatibility
- Maintain ORF storage format for new tuple versions
- Support conversion between heap and ORF formats during operations
- Preserve existing tuple chains for visibility

### 3. Performance Optimization
- Leverage ORF's optimized layout for better cache locality
- Minimize data movement and copying
- Support HOT (Heap-Only Tuple) updates when possible

### 4. WAL Integration
- Log operations for durability and replication
- Follow PostgreSQL's WAL record standards
- Support crash recovery and point-in-time recovery

## Function Specifications

### 1. optimized_tuple_delete()

**Purpose**: Mark a tuple as deleted using MVCC principles.

**Signature**:
```c
static TM_Result
optimized_tuple_delete(Relation relation, ItemPointer tid, CommandId cid,
                      Snapshot crosscheck, bool wait, TM_FailureData *tmfd, 
                      bool changingPart);
```

**Algorithm**:
```c
TM_Result optimized_tuple_delete(Relation relation, ItemPointer tid, CommandId cid,
                                Snapshot crosscheck, bool wait, TM_FailureData *tmfd, 
                                bool changingPart)
{
    TransactionId xid = GetCurrentTransactionId();
    Buffer buffer;
    Page page;
    ItemId lp;
    OptimizedTupleHeader tuple_header;
    TM_Result result;
    
    // 1. TUPLE LOCATION AND LOCKING
    // Get the page containing the tuple
    BlockNumber block = ItemPointerGetBlockNumber(tid);
    buffer = ReadBuffer(relation, block);
    page = BufferGetPage(buffer);
    
    // Handle visibility map pinning if page is all-visible
    Buffer vmbuffer = InvalidBuffer;
    if (PageIsAllVisible(page))
        visibilitymap_pin(relation, block, &vmbuffer);
    
    // Acquire exclusive lock on buffer
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    
    // 2. TUPLE RETRIEVAL AND VALIDATION
    OffsetNumber offnum = ItemPointerGetOffsetNumber(tid);
    lp = PageGetItemId(page, offnum);
    
    if (!ItemIdIsNormal(lp))
    {
        result = TM_Deleted;  // Tuple already deleted
        goto cleanup;
    }
    
    // Get the optimized tuple header (standard HeapTupleHeader)
    tuple_header = (OptimizedTupleHeader) PageGetItem(page, lp);
    
    // 3. VISIBILITY AND CONCURRENCY CHECKING
    // Create a HeapTupleData for visibility checking
    HeapTupleData tuple_data;
    tuple_data.t_len = ItemIdGetLength(lp);
    tuple_data.t_data = (HeapTupleHeader) tuple_header;
    tuple_data.t_self = *tid;
    tuple_data.t_tableOid = RelationGetRelid(relation);
    
    // Check if we can delete this tuple version
    result = HeapTupleSatisfiesUpdate(&tuple_data, cid, buffer);
    
    switch (result)
    {
        case TM_Ok:
            // Tuple is visible and can be deleted
            break;
            
        case TM_BeingModified:
            // Another transaction is modifying this tuple
            if (wait)
            {
                // Wait for the other transaction and retry
                // (Implementation follows heap_delete pattern)
                goto l1;  // Retry loop
            }
            else
            {
                goto cleanup;
            }
            
        case TM_Updated:
        case TM_Deleted:
            // Tuple already modified/deleted
            goto cleanup;
            
        default:
            elog(ERROR, "unrecognized HeapTupleSatisfiesUpdate status: %u", result);
    }
    
    // 4. TUPLE LOCKING (if needed)
    // Acquire tuple lock to prevent concurrent modifications
    // (Implementation follows heap_delete lock acquisition)
    
    // 5. CRITICAL SECTION - ATOMIC DELETION
    START_CRIT_SECTION();
    
    // Mark tuple as deleted by setting xmax
    HeapTupleHeaderSetXmax(tuple_header, xid);
    HeapTupleHeaderSetCmax(tuple_header, cid, false);
    
    // Update hint bits for performance
    tuple_header->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
    tuple_header->t_infomask2 &= ~HEAP2_XACT_MASK;
    tuple_header->t_infomask |= HEAP_XMAX_VALID;
    
    // 6. VISIBILITY MAP HANDLING
    bool all_visible_cleared = false;
    if (PageIsAllVisible(page))
    {
        all_visible_cleared = true;
        PageClearAllVisible(page);
        visibilitymap_clear(relation, block, vmbuffer, VISIBILITYMAP_VALID_BITS);
    }
    
    // 7. WAL LOGGING
    if (RelationNeedsWAL(relation))
    {
        xl_heap_delete xlrec;
        xlrec.flags = 0;
        if (all_visible_cleared)
            xlrec.flags |= XLH_DELETE_ALL_VISIBLE_CLEARED;
        xlrec.infobits_set = compute_infobits(tuple_header->t_infomask,
                                             tuple_header->t_infomask2);
        
        XLogRecPtr recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_DELETE, &xlrec, sizeof(xlrec));
        PageSetLSN(page, recptr);
    }
    
    // Mark buffer as dirty
    MarkBufferDirty(buffer);
    
    END_CRIT_SECTION();
    
    result = TM_Ok;
    
cleanup:
    if (BufferIsValid(vmbuffer))
        ReleaseBuffer(vmbuffer);
    UnlockReleaseBuffer(buffer);
    
    return result;
}
```

**Key Design Decisions**:
1. **Reuse Heap Visibility Logic**: Leverage `HeapTupleSatisfiesUpdate()` by creating a temporary `HeapTupleData` structure
2. **Standard MVCC Marking**: Use `xmax` field to mark deletion, maintaining full PostgreSQL compatibility
3. **WAL Compatibility**: Use existing heap WAL record formats for deletion operations
4. **Visibility Map Integration**: Properly clear visibility map bits when deleting visible tuples

### 2. optimized_tuple_update()

**Purpose**: Update a tuple by creating a new optimized version and marking the old one as updated.

**Signature**:
```c
static TM_Result
optimized_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
                      CommandId cid, Snapshot crosscheck, bool wait,
                      TM_FailureData *tmfd, LockTupleMode *lockmode,
                      TU_UpdateIndexes *update_indexes);
```

**Algorithm**:
```c
TM_Result optimized_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
                                CommandId cid, Snapshot crosscheck, bool wait,
                                TM_FailureData *tmfd, LockTupleMode *lockmode,
                                TU_UpdateIndexes *update_indexes)
{
    TransactionId xid = GetCurrentTransactionId();
    Buffer buffer, newbuf;
    TM_Result result;
    bool use_hot_update = false;
    
    // 1. ATTRIBUTE ANALYSIS FOR HOT UPDATES
    // Determine which attributes are being modified to decide on HOT update eligibility
    Bitmapset *hot_attrs = RelationGetIndexAttrBitmap(relation, INDEX_ATTR_BITMAP_HOT);
    Bitmapset *key_attrs = RelationGetIndexAttrBitmap(relation, INDEX_ATTR_BITMAP_KEY);
    Bitmapset *id_attrs = RelationGetIndexAttrBitmap(relation, INDEX_ATTR_BITMAP_IDENTITY_KEY);
    Bitmapset *modified_attrs = NULL;
    
    // Analyze which attributes changed (implementation similar to heap_update)
    // ...
    
    // 2. OLD TUPLE RETRIEVAL AND LOCKING
    BlockNumber block = ItemPointerGetBlockNumber(otid);
    buffer = ReadBuffer(relation, block);
    
    // Lock and validate old tuple (similar to delete operation)
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    
    Page page = BufferGetPage(buffer);
    OffsetNumber offnum = ItemPointerGetOffsetNumber(otid);
    ItemId lp = PageGetItemId(page, offnum);
    
    if (!ItemIdIsNormal(lp))
    {
        result = TM_Deleted;
        goto cleanup;
    }
    
    OptimizedTupleHeader old_header = (OptimizedTupleHeader) PageGetItem(page, lp);
    
    // Create HeapTupleData for visibility checking
    HeapTupleData oldtup;
    oldtup.t_len = ItemIdGetLength(lp);
    oldtup.t_data = (HeapTupleHeader) old_header;
    oldtup.t_self = *otid;
    oldtup.t_tableOid = RelationGetRelid(relation);
    
    // 3. VISIBILITY AND CONCURRENCY CHECKING
    result = HeapTupleSatisfiesUpdate(&oldtup, cid, buffer);
    if (result != TM_Ok)
    {
        // Handle concurrency conflicts (similar to heap_update)
        goto cleanup;
    }
    
    // 4. NEW TUPLE PREPARATION
    // Convert the slot data to optimized format
    Size new_tuple_size;
    HeapTuple new_optimized_tuple = build_optimized_tuple_from_slot(relation, slot, &new_tuple_size);
    
    // 5. HOT UPDATE DECISION
    // Check if we can do a HOT update (same page, no indexed attributes changed)
    bool key_intact = !bms_overlap(modified_attrs, key_attrs);
    Size pagefree = PageGetHeapFreeSpace(page);
    bool hot_attrs_modified = bms_overlap(modified_attrs, hot_attrs);
    
    if (key_intact && 
        !hot_attrs_modified && 
        new_tuple_size <= pagefree &&
        !RelationGetForm(relation)->relhasindex)  // Simplified for initial implementation
    {
        use_hot_update = true;
        newbuf = buffer;  // Same page
    }
    else
    {
        // Find a new page for the updated tuple
        newbuf = RelationGetBufferForTuple(relation, new_tuple_size,
                                          InvalidBuffer, 0, NULL, NULL, NULL, 0);
    }
    
    // 6. CRITICAL SECTION - ATOMIC UPDATE
    START_CRIT_SECTION();
    
    // Mark old tuple as updated
    HeapTupleHeaderSetXmax(old_header, xid);
    HeapTupleHeaderSetCmax(old_header, cid, false);
    old_header->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
    old_header->t_infomask2 &= ~HEAP2_XACT_MASK;
    old_header->t_infomask |= HEAP_XMAX_VALID;
    
    if (use_hot_update)
        old_header->t_infomask2 |= HEAP2_HOT_UPDATED;
    
    // Place new tuple
    OffsetNumber new_offnum;
    if (newbuf == buffer)
    {
        // HOT update - same page
        new_offnum = PageAddItem(page, (Item) new_optimized_tuple->t_data, 
                               new_optimized_tuple->t_len, InvalidOffsetNumber, 
                               false, true);
    }
    else
    {
        // Different page
        Page newpage = BufferGetPage(newbuf);
        new_offnum = PageAddItem(newpage, (Item) new_optimized_tuple->t_data,
                               new_optimized_tuple->t_len, InvalidOffsetNumber,
                               false, true);
    }
    
    if (new_offnum == InvalidOffsetNumber)
        elog(ERROR, "failed to add new tuple to page");
    
    // Set up tuple chain (ctid pointer from old to new)
    ItemPointer new_tid = &new_optimized_tuple->t_self;
    ItemPointerSet(new_tid, BufferGetBlockNumber(newbuf), new_offnum);
    HeapTupleHeaderSetCtid(old_header, new_tid);
    
    // Update new tuple header
    OptimizedTupleHeader new_header = (OptimizedTupleHeader) new_optimized_tuple->t_data;
    HeapTupleHeaderSetXmin(new_header, xid);
    HeapTupleHeaderSetCmin(new_header, cid);
    HeapTupleHeaderSetXmax(new_header, 0);
    new_header->t_infomask |= HEAP_XMAX_INVALID;
    
    if (use_hot_update)
        new_header->t_infomask2 |= HEAP2_HEAP_ONLY;
    
    // 7. VISIBILITY MAP AND WAL HANDLING
    // Handle visibility maps for both old and new pages
    // Log the update operation to WAL
    // (Implementation follows heap_update pattern)
    
    // Mark buffers dirty
    MarkBufferDirty(buffer);
    if (newbuf != buffer)
        MarkBufferDirty(newbuf);
    
    END_CRIT_SECTION();
    
    // 8. UPDATE SLOT WITH NEW TID
    slot->tts_tid = *new_tid;
    slot->tts_tableOid = RelationGetRelid(relation);
    
    result = TM_Ok;
    
cleanup:
    // Cleanup buffers and locks
    return result;
}
```

**Key Design Decisions**:
1. **HOT Update Support**: Implement HOT updates when indexed attributes aren't modified and space permits
2. **Format Preservation**: New tuple versions maintain ORF optimized format
3. **Tuple Chain Maintenance**: Properly link old and new tuple versions via ctid pointers
4. **Slot Integration**: Update the TupleTableSlot with new tuple location

### 3. Helper Functions

#### build_optimized_tuple_from_slot()
```c
static HeapTuple
build_optimized_tuple_from_slot(Relation relation, TupleTableSlot *slot, Size *tuple_size)
{
    TupleDesc tupdesc = RelationGetDescr(relation);
    OptimizedColumnMapCache *cache = get_or_build_column_cache(relation);
    
    // Extract all attributes from slot
    bool *isnull = (bool *) palloc(tupdesc->natts * sizeof(bool));
    Datum *values = (Datum *) palloc(tupdesc->natts * sizeof(Datum));
    
    slot_getallattrs(slot);
    memcpy(values, slot->tts_values, tupdesc->natts * sizeof(Datum));
    memcpy(isnull, slot->tts_isnull, tupdesc->natts * sizeof(bool));
    
    // Build optimized tuple using the same logic as optimized_tuple_insert
    return build_optimized_tuple(relation, values, isnull, cache, tuple_size);
}
```

#### optimized_tuple_satisfies_update()
```c
static TM_Result
optimized_tuple_satisfies_update(Relation relation, ItemPointer tid, CommandId cid,
                                Buffer buffer, OptimizedTupleHeader tuple_header)
{
    // Convert to HeapTupleData for standard visibility checking
    HeapTupleData tuple_data;
    tuple_data.t_len = /* calculate from page item */;
    tuple_data.t_data = (HeapTupleHeader) tuple_header;
    tuple_data.t_self = *tid;
    tuple_data.t_tableOid = RelationGetRelid(relation);
    
    return HeapTupleSatisfiesUpdate(&tuple_data, cid, buffer);
}
```

## WAL Integration

### WAL Record Types
The ORF will reuse existing heap WAL record types for compatibility:

1. **XLOG_HEAP_DELETE**: For delete operations
2. **XLOG_HEAP_UPDATE**: For update operations  
3. **XLOG_HEAP_HOT_UPDATE**: For HOT update operations

### WAL Record Structure
```c
// For deletes - reuse xl_heap_delete
typedef struct xl_heap_delete
{
    TransactionId xmax;
    OffsetNumber offnum;
    uint8 infobits_set;
    uint8 flags;
} xl_heap_delete;

// For updates - reuse xl_heap_update  
typedef struct xl_heap_update
{
    TransactionId old_xmax;
    OffsetNumber old_offnum;
    uint8 old_infobits_set;
    uint8 flags;
    TransactionId new_xmax;
    OffsetNumber new_offnum;
    /* NEW TUPLE DATA FOLLOWS */
} xl_heap_update;
```

### Recovery Procedures
During crash recovery, the ORF must:

1. **Parse standard heap WAL records**: Reuse existing heap recovery functions
2. **Reconstruct optimized tuples**: Apply WAL records to recreate ORF tuple format
3. **Maintain tuple chains**: Ensure ctid pointers are correctly restored
4. **Update visibility maps**: Restore visibility information

## Performance Considerations

### 1. Cache Utilization
- **Column Cache**: Leverage existing `OptimizedColumnMapCache` for O(1) attribute access
- **Buffer Locality**: ORF's layout improves cache performance during updates

### 2. Memory Management
- **Minimize Copying**: Reuse tuple data structures where possible
- **Efficient Conversion**: Optimize slot-to-ORF and ORF-to-slot conversions

### 3. Concurrency Optimization
- **Lock Duration**: Minimize buffer lock hold times
- **HOT Updates**: Aggressive use of HOT updates to reduce index maintenance overhead

## Error Handling and Edge Cases

### 1. Concurrency Conflicts
- **Lock Waits**: Implement proper wait/retry logic for concurrent modifications
- **Deadlock Detection**: Integrate with PostgreSQL's deadlock detection system

### 2. Space Management  
- **Page Full Conditions**: Handle cases where updates can't fit on current page
- **TOAST Integration**: Ensure proper handling of large/external values

### 3. Transaction Isolation
- **Snapshot Isolation**: Maintain proper MVCC visibility across all isolation levels
- **Serializable Conflicts**: Detect and handle serialization conflicts

## Implementation Plan

### Phase 1: Basic Delete Operation
1. Implement `optimized_tuple_delete()` with minimal feature set
2. Add WAL logging for delete operations
3. Register delete function in TableAmRoutine
4. Basic testing with simple delete scenarios

### Phase 2: Basic Update Operation
1. Implement `optimized_tuple_update()` without HOT updates
2. Add WAL logging for update operations
3. Implement tuple format conversion helpers
4. Register update function in TableAmRoutine

### Phase 3: Advanced Features
1. Add HOT update support
2. Optimize performance critical paths
3. Add comprehensive error handling
4. Performance benchmarking and optimization

### Phase 4: Production Readiness
1. Extensive concurrency testing
2. Edge case handling
3. Recovery testing
4. Documentation and examples

## Testing Strategy

### 1. Functional Testing
- **Basic DML**: Simple UPDATE/DELETE operations
- **Concurrency**: Multi-transaction scenarios
- **Recovery**: Crash recovery and WAL replay

### 2. Performance Testing
- **Throughput**: Compare against standard heap performance
- **Cache Efficiency**: Measure cache hit rates and locality
- **Memory Usage**: Profile memory allocation patterns

### 3. Compatibility Testing
- **PostgreSQL Features**: Ensure compatibility with triggers, constraints, etc.
- **Replication**: Test logical and physical replication
- **Extensions**: Compatibility with other PostgreSQL extensions

## Conclusion

This design provides a comprehensive approach to implementing UPDATE and DELETE operations for the optimized_row_format extension. The design prioritizes:

1. **Full MVCC Compatibility**: Ensuring seamless integration with PostgreSQL's transaction system
2. **Performance Optimization**: Leveraging ORF's storage advantages
3. **Maintainability**: Following established PostgreSQL patterns and code organization
4. **Extensibility**: Building a foundation for future optimizations

The implementation will follow PostgreSQL's established patterns while optimizing for the unique characteristics of the ORF storage format, providing users with both performance benefits and full PostgreSQL feature compatibility.
