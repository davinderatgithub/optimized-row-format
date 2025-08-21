# DML and Indexing Support Design Document

**Date**: 2025-08-19  
**Engineer**: engineer_01  
**Task**: T3 - Research and Design DML and Indexing Support  
**Work Item**: Personal-3-Analyze-ORF-Performance

## Executive Summary

Based on comprehensive analysis of PostgreSQL's heap access method (`heapam.c` and `heapam_handler.c`), this document outlines the implementation strategy for complete DML (UPDATE/DELETE) and indexing support in the optimized row format extension.

## Current Status Assessment

### ✅ **Implemented and Working**
- **INSERT operations**: Fully functional with 16-bit offset optimization
- **SELECT operations**: Optimized with 4.75x-6.22x performance vs heap
- **Basic table operations**: CREATE TABLE, data storage, retrieval
- **Storage efficiency**: 11-12% overhead for wide tables (target: <20%)

### ❌ **Missing Critical Functionality**
- **UPDATE operations**: Currently delegates to heap (fails with data corruption)
- **DELETE operations**: Currently delegates to heap (fails with data corruption)
- **Index support**: Missing index_fetch_* functions (PRIMARY KEY creation fails)
- **MVCC compliance**: Limited transaction visibility handling

## Research Analysis

### PostgreSQL Heap Implementation Patterns

From analysis of `src/backend/access/heap/heapam.c` and `heapam_handler.c`:

#### 1. **DELETE Operation Pattern** (`heap_delete`)
```c
TM_Result heap_delete(Relation relation, ItemPointer tid, 
                     CommandId cid, Snapshot crosscheck, bool wait,
                     TM_FailureData *tmfd, bool changingPart)
{
    // 1. Get buffer and page for target tuple
    block = ItemPointerGetBlockNumber(tid);
    buffer = ReadBuffer(relation, block);
    page = BufferGetPage(buffer);
    
    // 2. Lock buffer and get tuple
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
    tp.t_data = (HeapTupleHeader) PageGetItem(page, lp);
    
    // 3. Check MVCC visibility
    result = HeapTupleSatisfiesUpdate(&tp, cid, buffer);
    
    // 4. Mark tuple as deleted (CRITICAL OPERATION)
    HeapTupleHeaderSetXmax(tp.t_data, new_xmax);
    HeapTupleHeaderSetCmax(tp.t_data, cid, iscombo);
    
    // 5. WAL logging and cleanup
    // ...
}
```

#### 2. **UPDATE Operation Pattern** (`heap_update`)
```c
TM_Result heap_update(Relation relation, ItemPointer otid, HeapTuple newtup,
                     CommandId cid, Snapshot crosscheck, bool wait,
                     TM_FailureData *tmfd, LockTupleMode *lockmode,
                     TU_UpdateIndexes *update_indexes)
{
    // 1. Locate and lock old tuple (similar to DELETE)
    // 2. Check MVCC visibility
    // 3. Determine if HOT update is possible
    // 4. Mark old tuple as deleted (set xmax)
    // 5. Insert new tuple version
    // 6. Link old tuple to new tuple via t_ctid
    // 7. Update indexes if necessary
}
```

#### 3. **Index Support Pattern** (`heapam_index_fetch_*`)
```c
// Initialize index scan
static IndexFetchTableData *heapam_index_fetch_begin(Relation rel)
{
    IndexFetchHeapData *hscan = palloc0(sizeof(IndexFetchHeapData));
    hscan->xs_base.rel = rel;
    hscan->xs_cbuf = InvalidBuffer;
    return &hscan->xs_base;
}

// Core index fetch function
static bool heapam_index_fetch_tuple(IndexFetchTableData *scan,
                                   ItemPointer tid, Snapshot snapshot,
                                   TupleTableSlot *slot,
                                   bool *call_again, bool *all_dead)
{
    // 1. Switch to correct buffer if needed
    // 2. Lock buffer for reading
    // 3. Search for tuple using heap_hot_search_buffer
    // 4. Store tuple in slot if found
    // 5. Handle HOT chain traversal
}
```

## Implementation Design

### Phase 1: DELETE Operation Implementation

#### Function: `optimized_tuple_delete`

**Signature**:
```c
static TM_Result
optimized_tuple_delete(Relation relation, ItemPointer tid,
                      CommandId cid, Snapshot crosscheck, bool wait,
                      TM_FailureData *tmfd, bool changingPart)
```

**Implementation Strategy**:
```c
static TM_Result
optimized_tuple_delete(Relation relation, ItemPointer tid,
                      CommandId cid, Snapshot crosscheck, bool wait,
                      TM_FailureData *tmfd, bool changingPart)
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

    // 4. MVCC COMPLIANCE: Check tuple visibility and update status
    result = HeapTupleSatisfiesUpdate(&tp, cid, buffer);
    
    if (result == TM_Invisible) {
        UnlockReleaseBuffer(buffer);
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                       errmsg("attempted to delete invisible tuple")));
    }
    
    if (result != TM_Ok) {
        UnlockReleaseBuffer(buffer);
        return result; // Tuple being modified, deleted, etc.
    }

    // 5. DELETION LOGIC: Mark tuple as deleted
    new_xmax = xid;
    new_infomask = tp.t_data->t_infomask;
    new_infomask2 = tp.t_data->t_infomask2;
    
    // Compute new infomask bits for deletion
    compute_new_xmax_infomask(new_xmax, tp.t_data->t_infomask,
                             tp.t_data->t_infomask2, xid, LockTupleExclusive,
                             false, &new_xmax, &new_infomask, &new_infomask2);

    START_CRIT_SECTION();

    // 6. ATOMIC UPDATE: Set deletion markers
    tp.t_data->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
    tp.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
    tp.t_data->t_infomask |= new_infomask;
    tp.t_data->t_infomask2 |= new_infomask2;
    HeapTupleHeaderClearHotUpdated(tp.t_data);
    HeapTupleHeaderSetXmax(tp.t_data, new_xmax);
    HeapTupleHeaderSetCmax(tp.t_data, cid, iscombo);

    MarkBufferDirty(buffer);

    // 7. WAL LOGGING: Record deletion for crash recovery
    if (RelationNeedsWAL(relation)) {
        xl_heap_delete xlrec;
        XLogRecPtr  recptr;

        xlrec.flags = 0;
        xlrec.infobits_set = compute_infobits(tp.t_data->t_infomask,
                                            tp.t_data->t_infomask2);
        xlrec.offnum = ItemPointerGetOffsetNumber(&tp.t_self);
        xlrec.xmax = new_xmax;

        XLogBeginInsert();
        XLogRegisterData((char *) &xlrec, SizeOfHeapDelete);
        XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

        recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_DELETE);
        PageSetLSN(page, recptr);
    }

    END_CRIT_SECTION();

    LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
    
    // 8. CLEANUP AND STATISTICS
    if (vmbuffer != InvalidBuffer)
        ReleaseBuffer(vmbuffer);
    ReleaseBuffer(buffer);

    pgstat_count_heap_delete(relation);

    return TM_Ok;
}
```

**Key Design Decisions**:
1. **MVCC Compliance**: Full `HeapTupleSatisfiesUpdate` checking
2. **Atomic Operations**: Critical section protection for tuple modification
3. **WAL Logging**: Crash recovery support (essential for production)
4. **Buffer Management**: Proper locking and release patterns
5. **Statistics**: Integration with PostgreSQL's statistics system

### Phase 2: UPDATE Operation Implementation

#### Function: `optimized_tuple_update`

**Implementation Strategy**:
UPDATE is implemented as a **delete-then-insert** operation with additional complexity:

```c
static TM_Result
optimized_tuple_update(Relation relation, ItemPointer otid, HeapTuple newtup,
                      CommandId cid, Snapshot crosscheck, bool wait,
                      TM_FailureData *tmfd, LockTupleMode *lockmode,
                      TU_UpdateIndexes *update_indexes)
{
    // 1. LOCATE OLD TUPLE: Similar to delete operation
    // 2. MVCC VALIDATION: Check update permissions
    // 3. HOT UPDATE ANALYSIS: Determine if indexes need updating
    // 4. SPACE ALLOCATION: Find space for new tuple version
    // 5. OLD TUPLE MARKING: Mark as deleted, link to new version
    // 6. NEW TUPLE INSERTION: Use existing optimized_tuple_insert logic
    // 7. INDEX MAINTENANCE: Update indexes if necessary
    // 8. WAL LOGGING: Record both old and new tuple information
}
```

**Critical Components**:

1. **HOT (Heap-Only Tuple) Update Detection**:
   ```c
   // Determine if any indexed columns changed
   bool hot_attrs_changed = false;
   Bitmapset *hot_attrs = RelationGetIndexAttrBitmap(relation, INDEX_ATTR_BITMAP_HOT);
   
   // If no indexed columns changed, can do HOT update (faster)
   if (!heap_tuple_attr_equals(tupdesc, hot_attrs, &oldtup, newtup)) {
       hot_attrs_changed = true;
       *update_indexes = TU_All; // Must update all indexes
   } else {
       *update_indexes = TU_None; // HOT update, no index changes needed
   }
   ```

2. **Tuple Linking**:
   ```c
   // Link old tuple to new tuple location
   HeapTupleHeaderSetCmax(oldtup.t_data, cid, iscombo);
   oldtup.t_data->t_ctid = newtup->t_self; // Point to new version
   ```

3. **New Tuple Insertion**:
   ```c
   // Reuse existing optimized tuple insertion logic
   // but with UPDATE-specific transaction handling
   HeapTupleHeaderSetXmin(newtup->t_data, xid);
   HeapTupleHeaderSetCmin(newtup->t_data, cid);
   HeapTupleHeaderSetXmax(newtup->t_data, 0); // New tuple not deleted
   ```

### Phase 3: Index Support Implementation

#### Data Structure: `OptimizedIndexFetchData`

```c
typedef struct OptimizedIndexFetchData
{
    IndexFetchTableData xs_base;    /* Base structure */
    Buffer      xs_cbuf;            /* Current buffer */
    OptimizedColumnMapCache *column_cache;  /* Cached column mappings */
} OptimizedIndexFetchData;
```

#### Function: `optimized_index_fetch_begin`

```c
static IndexFetchTableData *
optimized_index_fetch_begin(Relation rel)
{
    OptimizedIndexFetchData *oscan = palloc0(sizeof(OptimizedIndexFetchData));
    
    oscan->xs_base.rel = rel;
    oscan->xs_cbuf = InvalidBuffer;
    
    // Initialize column cache for efficient attribute access
    oscan->column_cache = get_or_build_column_cache(RelationGetDescr(rel));
    
    return &oscan->xs_base;
}
```

#### Function: `optimized_index_fetch_tuple` (Core Function)

```c
static bool
optimized_index_fetch_tuple(struct IndexFetchTableData *scan,
                           ItemPointer tid, Snapshot snapshot,
                           TupleTableSlot *slot,
                           bool *call_again, bool *all_dead)
{
    OptimizedIndexFetchData *oscan = (OptimizedIndexFetchData *) scan;
    OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
    bool got_tuple;
    HeapTupleData tuple;
    
    Assert(TTS_IS_CUSTOM(slot)); // Ensure we have our custom slot type

    // 1. BUFFER MANAGEMENT: Switch to correct page if needed
    if (!*call_again) {
        Buffer prev_buf = oscan->xs_cbuf;
        
        oscan->xs_cbuf = ReleaseAndReadBuffer(oscan->xs_cbuf,
                                            oscan->xs_base.rel,
                                            ItemPointerGetBlockNumber(tid));
        
        // Page pruning optimization for new pages
        if (prev_buf != oscan->xs_cbuf)
            heap_page_prune_opt(oscan->xs_base.rel, oscan->xs_cbuf);
    }

    // 2. TUPLE LOCATION: Lock buffer and search for tuple
    LockBuffer(oscan->xs_cbuf, BUFFER_LOCK_SHARE);
    
    got_tuple = optimized_hot_search_buffer(tid,
                                          oscan->xs_base.rel,
                                          oscan->xs_cbuf,
                                          snapshot,
                                          &tuple,
                                          all_dead,
                                          !*call_again);
    
    tuple.t_self = *tid;
    LockBuffer(oscan->xs_cbuf, BUFFER_LOCK_UNLOCK);

    // 3. SLOT POPULATION: Store tuple in optimized slot
    if (got_tuple) {
        *call_again = !IsMVCCSnapshot(snapshot);
        
        ExecClearTuple(slot);
        
        // Store raw optimized tuple for on-demand attribute extraction
        opt_slot->opt_tuple = heap_copytuple(&tuple);
        opt_slot->column_cache = oscan->column_cache;
        
        // Initialize slot cache for fast access
        optimized_slot_init_cache(opt_slot);
        
        slot->tts_tableOid = RelationGetRelid(scan->rel);
        slot->tts_flags &= ~TTS_FLAG_EMPTY;
        slot->tts_nvalid = 0; // Attributes will be extracted on-demand
        
    } else {
        *call_again = false;
    }

    return got_tuple;
}
```

#### Supporting Function: `optimized_hot_search_buffer`

```c
static bool
optimized_hot_search_buffer(ItemPointer tid, Relation relation, Buffer buffer,
                           Snapshot snapshot, HeapTuple heaptup,
                           bool *all_dead, bool first_call)
{
    Page page = BufferGetPage(buffer);
    OffsetNumber offnum = ItemPointerGetOffsetNumber(tid);
    bool valid;
    
    // Basic implementation - can be enhanced with HOT chain following
    ItemId lp = PageGetItemId(page, offnum);
    
    if (!ItemIdIsNormal(lp)) {
        *all_dead = false;
        return false;
    }
    
    heaptup->t_data = (HeapTupleHeader) PageGetItem(page, lp);
    heaptup->t_len = ItemIdGetLength(lp);
    heaptup->t_tableOid = RelationGetRelid(relation);
    heaptup->t_self = *tid;
    
    // Check visibility
    valid = HeapTupleSatisfiesVisibility(heaptup, snapshot, buffer);
    
    if (valid) {
        *all_dead = false;
        return true;
    }
    
    // Could implement HOT chain following here for optimization
    *all_dead = HeapTupleIsSurelyDead(heaptup, GlobalVisTestFor(relation));
    return false;
}
```

#### Function: `optimized_index_fetch_end`

```c
static void
optimized_index_fetch_end(IndexFetchTableData *scan)
{
    OptimizedIndexFetchData *oscan = (OptimizedIndexFetchData *) scan;
    
    // Release current buffer if held
    if (BufferIsValid(oscan->xs_cbuf)) {
        ReleaseBuffer(oscan->xs_cbuf);
        oscan->xs_cbuf = InvalidBuffer;
    }
    
    // Don't free column_cache - it's shared and managed elsewhere
    
    pfree(oscan);
}
```

### Phase 4: Table AM Integration

#### Update `optimized_tableam` Structure

```c
static const TableAmRoutine optimized_tableam = {
    .type = T_TableAmRoutine,

    // ... existing functions ...

    // DML OPERATIONS
    .tuple_delete = optimized_tuple_delete,
    .tuple_update = optimized_tuple_update,
    
    // INDEX SUPPORT
    .index_fetch_begin = optimized_index_fetch_begin,
    .index_fetch_reset = optimized_index_fetch_reset, 
    .index_fetch_end = optimized_index_fetch_end,
    .index_fetch_tuple = optimized_index_fetch_tuple,
    
    // ... rest of existing functions ...
};
```

#### Add Missing Helper Functions

```c
static void
optimized_index_fetch_reset(IndexFetchTableData *scan)
{
    OptimizedIndexFetchData *oscan = (OptimizedIndexFetchData *) scan;
    
    if (BufferIsValid(oscan->xs_cbuf)) {
        ReleaseBuffer(oscan->xs_cbuf);
        oscan->xs_cbuf = InvalidBuffer;
    }
}
```

## Implementation Roadmap

### Week 1: DELETE Operation (High Priority)
1. **Day 1-2**: Implement `optimized_tuple_delete` function
2. **Day 3**: Add WAL logging support for DELETE operations
3. **Day 4**: Create comprehensive DELETE test cases
4. **Day 5**: Debug and validate MVCC compliance

**Success Criteria**:
- `DELETE FROM optimized_table WHERE condition` works correctly
- Transaction isolation respected
- No data corruption or server crashes

### Week 2: UPDATE Operation (High Priority)  
1. **Day 1-3**: Implement `optimized_tuple_update` function
2. **Day 4**: Add HOT update optimization logic
3. **Day 5**: Integrate with existing `optimized_tuple_insert`

**Success Criteria**:
- `UPDATE optimized_table SET column = value WHERE condition` works
- Both simple and complex updates handled
- Index maintenance working (when indexes implemented)

### Week 3: Index Support (Medium Priority)
1. **Day 1-2**: Implement index fetch functions (`*_begin`, `*_tuple`, `*_end`)
2. **Day 3**: Add `OptimizedIndexFetchData` structure and helpers
3. **Day 4-5**: Test PRIMARY KEY creation and index-based queries

**Success Criteria**:
- `ALTER TABLE optimized_table ADD PRIMARY KEY (id)` succeeds  
- `SELECT * FROM optimized_table WHERE id = 123` uses index scan
- Index scan performance competitive with heap

### Week 4: Integration and Testing
1. **Day 1-2**: Update `TableAmRoutine` with all new functions
2. **Day 3**: Comprehensive integration testing
3. **Day 4**: Performance benchmarking vs heap
4. **Day 5**: Documentation and cleanup

**Success Criteria**:
- All DML operations work reliably
- Index operations functional
- Performance meets or exceeds heap for target use cases

## Risk Assessment and Mitigation

### High Risk Areas

1. **MVCC Compliance Bugs**
   - **Risk**: Incorrect transaction visibility handling
   - **Mitigation**: Extensive testing with concurrent transactions
   - **Testing**: Multi-transaction workloads, isolation level tests

2. **Buffer Management Issues**
   - **Risk**: Buffer leaks or deadlocks during concurrent operations
   - **Mitigation**: Follow PostgreSQL buffer management patterns exactly
   - **Testing**: High concurrency stress tests

3. **WAL Logging Correctness**
   - **Risk**: Incomplete crash recovery, data loss
   - **Mitigation**: Implement WAL logging identical to heap patterns
   - **Testing**: Crash recovery simulation, pg_resetwal testing

### Medium Risk Areas

1. **Performance Regressions**
   - **Risk**: DML operations slower than heap
   - **Mitigation**: Profile and optimize hot paths
   - **Testing**: Comprehensive performance benchmarking

2. **Index Integration Complexity**
   - **Risk**: Complex interactions with PostgreSQL's index subsystem
   - **Mitigation**: Start with simple index types, expand gradually
   - **Testing**: Multiple index types (B-tree, Hash, GiST)

## Testing Strategy

### Unit Tests
```sql
-- DELETE operation tests
CREATE TABLE test_delete (id INT, data TEXT) USING optimized_row_format;
INSERT INTO test_delete VALUES (1, 'test'), (2, 'data');
DELETE FROM test_delete WHERE id = 1;
SELECT * FROM test_delete; -- Should return only (2, 'data')

-- UPDATE operation tests  
CREATE TABLE test_update (id INT, counter INT) USING optimized_row_format;
INSERT INTO test_update VALUES (1, 10), (2, 20);
UPDATE test_update SET counter = counter + 1 WHERE id = 1;
SELECT * FROM test_update WHERE id = 1; -- Should return (1, 11)

-- Index operation tests
CREATE TABLE test_index (id INT PRIMARY KEY, name TEXT) USING optimized_row_format;
INSERT INTO test_index VALUES (1, 'Alice'), (2, 'Bob');
SELECT * FROM test_index WHERE id = 2; -- Should use index scan
```

### Concurrency Tests
```sql
-- Multi-transaction DELETE/UPDATE testing
-- Session 1: BEGIN; UPDATE test_table SET x = 1 WHERE id = 1;
-- Session 2: UPDATE test_table SET x = 2 WHERE id = 1; -- Should wait
-- Session 1: COMMIT; -- Session 2 should now proceed
```

### Performance Benchmarks
```sql
-- Compare DML performance against heap
-- Large table UPDATE performance
-- Index scan vs sequential scan performance
-- Concurrent transaction throughput
```

## Compatibility and Migration

### Backward Compatibility
- Existing optimized tables continue to work for INSERT/SELECT
- No changes to storage format required
- Graceful degradation if features not implemented

### Migration Path
1. **Phase 1**: Deploy DELETE support
2. **Phase 2**: Add UPDATE support  
3. **Phase 3**: Enable index support
4. **Phase 4**: Performance optimizations

## Success Metrics

### Functional Requirements
- [ ] `DELETE FROM optimized_table WHERE condition` works correctly
- [ ] `UPDATE optimized_table SET col = val WHERE condition` works correctly  
- [ ] `ALTER TABLE optimized_table ADD PRIMARY KEY (col)` succeeds
- [ ] Index-based queries use optimized table access

### Performance Requirements
- [ ] DELETE performance within 20% of heap performance
- [ ] UPDATE performance within 30% of heap performance (includes format overhead)
- [ ] Index scan performance competitive with heap
- [ ] No regressions in existing INSERT/SELECT performance

### Reliability Requirements
- [ ] ACID compliance maintained
- [ ] Crash recovery works correctly
- [ ] Concurrent transaction isolation preserved
- [ ] No memory leaks or buffer management issues

## Conclusion

This design provides a comprehensive roadmap for implementing complete DML and indexing support in the optimized row format extension. The approach follows PostgreSQL's established patterns while leveraging the extension's optimized storage format.

**Key Implementation Principles**:
1. **MVCC Compliance**: Strict adherence to PostgreSQL's transaction model
2. **Performance Focus**: Minimize overhead while adding functionality  
3. **Incremental Development**: Phased implementation with testing at each stage
4. **Compatibility**: Seamless integration with existing PostgreSQL features

**Expected Outcome**: A production-ready table access method that provides both storage optimization benefits and complete PostgreSQL feature compatibility.
