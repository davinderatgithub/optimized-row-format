# PostgreSQL Heap Access Method Implementation Analysis

This document provides a comprehensive analysis of PostgreSQL's heap access method implementation, focusing on the core functions responsible for tuple scanning, insertion, updates, and deletions. This analysis serves as a reference for implementing DML operations in the optimized_row_format extension.

## Overview

PostgreSQL's heap access method is implemented through a layered architecture:

1. **Table Access Method Interface** (`src/backend/access/heap/heapam_handler.c`)
2. **Core Heap Operations** (`src/backend/access/heap/heapam.c`)
3. **Supporting Modules** (visibility, buffer management, WAL logging)

The heap access method follows PostgreSQL's Multi-Version Concurrency Control (MVCC) model, where tuples are never updated in place but rather new versions are created with visibility controlled through transaction metadata.

## Core Functions Analysis

### 1. heap_getnext() - Tuple Scanning

**Location**: `src/backend/access/heap/heapam.c:1263`

**Purpose**: Retrieves the next tuple in a sequential scan.

**Key Architecture Points**:

```c
HeapTuple heap_getnext(TableScanDesc sscan, ScanDirection direction)
{
    HeapScanDesc scan = (HeapScanDesc) sscan;
    
    // Safety checks for table AM compatibility
    if (unlikely(sscan->rs_rd->rd_tableam != GetHeapamTableAmRoutine()))
        ereport(ERROR, ...);
        
    // Choose scanning strategy based on scan flags
    if (scan->rs_base.rs_flags & SO_ALLOW_PAGEMODE)
        heapgettup_pagemode(scan, direction, ...);
    else
        heapgettup(scan, direction, ...);
        
    // Return current tuple or NULL if scan complete
    return (scan->rs_ctup.t_data == NULL) ? NULL : &scan->rs_ctup;
}
```

**Core Implementation Details**:

- **Scan State Management**: Uses `HeapScanDesc` structure to maintain scan position
- **Page Mode vs. Tuple Mode**: Optimizes access pattern based on scan flags
- **Buffer Management**: Leverages PostgreSQL's shared buffer pool
- **Visibility Checking**: Each tuple checked for MVCC visibility during scan

**Key Helper Function - heapgettup()**:
```c
static void heapgettup(HeapScanDesc scan, ScanDirection dir, int nkeys, ScanKey key)
{
    // Continue from previous position or start new scan
    if (likely(scan->rs_inited)) {
        LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);
        page = heapgettup_continue_page(scan, dir, &linesleft, &lineoff);
    }
    
    // Main scanning loop
    while (true) {
        heap_fetch_next_buffer(scan, dir);
        if (!BufferIsValid(scan->rs_cbuf)) break;
        
        LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);
        
        // Scan tuples on current page
        for (; linesleft > 0; linesleft--, lineoff += dir) {
            ItemId lpp = PageGetItemId(page, lineoff);
            if (!ItemIdIsNormal(lpp)) continue;
            
            // Set tuple data and perform visibility check
            tuple->t_data = (HeapTupleHeader) PageGetItem(page, lpp);
            // ... visibility and key checking logic
        }
    }
}
```

### 2. heap_insert() - Tuple Insertion

**Location**: `src/backend/access/heap/heapam.c:2005`

**Purpose**: Inserts a new tuple into a heap relation.

**Key Architecture Points**:

```c
void heap_insert(Relation relation, HeapTuple tup, CommandId cid,
                 int options, BulkInsertState bistate)
{
    TransactionId xid = GetCurrentTransactionId();
    
    // 1. Prepare tuple (set transaction headers, handle TOAST)
    heaptup = heap_prepare_insert(relation, tup, xid, cid, options);
    
    // 2. Find suitable page with space
    buffer = RelationGetBufferForTuple(relation, heaptup->t_len, ...);
    
    // 3. Check for serialization conflicts
    CheckForSerializableConflictIn(relation, NULL, InvalidBlockNumber);
    
    // Critical section - no errors allowed
    START_CRIT_SECTION();
    
    // 4. Actually place tuple on page
    RelationPutHeapTuple(relation, buffer, heaptup, ...);
    
    // 5. Handle visibility map updates
    if (PageIsAllVisible(BufferGetPage(buffer))) {
        PageClearAllVisible(BufferGetPage(buffer));
        visibilitymap_clear(relation, ...);
    }
    
    // 6. Mark buffer dirty and log to WAL
    MarkBufferDirty(buffer);
    // WAL logging logic...
    
    END_CRIT_SECTION();
}
```

**Key Helper Function - heap_prepare_insert()**:
```c
static HeapTuple heap_prepare_insert(Relation relation, HeapTuple tup, 
                                   TransactionId xid, CommandId cid, int options)
{
    // Clear transaction flags and set new ones
    tup->t_data->t_infomask &= ~(HEAP_XACT_MASK);
    tup->t_data->t_infomask2 &= ~(HEAP2_XACT_MASK);
    tup->t_data->t_infomask |= HEAP_XMAX_INVALID;
    
    // Set transaction identifiers
    HeapTupleHeaderSetXmin(tup->t_data, xid);
    HeapTupleHeaderSetCmin(tup->t_data, cid);
    HeapTupleHeaderSetXmax(tup->t_data, 0);
    
    // Handle TOAST if tuple is too large
    if (HeapTupleHasExternal(tup) || tup->t_len > TOAST_TUPLE_THRESHOLD)
        return heap_toast_insert_or_update(relation, tup, NULL, options);
    
    return tup;
}
```

### 3. heap_delete() - Tuple Deletion

**Location**: `src/backend/access/heap/heapam.c:2694`

**Purpose**: Marks a tuple as deleted using MVCC principles.

**Key Architecture Points**:

```c
TM_Result heap_delete(Relation relation, ItemPointer tid, CommandId cid,
                     Snapshot crosscheck, bool wait, TM_FailureData *tmfd, 
                     bool changingPart)
{
    TransactionId xid = GetCurrentTransactionId();
    
    // 1. Locate tuple using TID
    block = ItemPointerGetBlockNumber(tid);
    buffer = ReadBuffer(relation, block);
    page = BufferGetPage(buffer);
    
    // 2. Pin visibility map if needed
    if (PageIsAllVisible(page))
        visibilitymap_pin(relation, block, &vmbuffer);
    
    // 3. Lock buffer and get tuple
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
    tp.t_data = (HeapTupleHeader) PageGetItem(page, lp);
    
    // 4. Check tuple visibility and acquire tuple lock
    result = HeapTupleSatisfiesUpdate(&tp, cid, buffer);
    // Handle various visibility states (OK, DELETED, UPDATED, etc.)
    
    // 5. Mark tuple as deleted by setting xmax
    HeapTupleHeaderSetXmax(tp.t_data, xid);
    HeapTupleHeaderSetCmax(tp.t_data, cid, iscombo);
    
    // 6. Update hint bits and handle visibility map
    // 7. WAL logging and buffer management
    
    return result;
}
```

### 4. heap_update() - Tuple Updates

**Location**: `src/backend/access/heap/heapam.c:3161`

**Purpose**: Updates a tuple by creating a new version and marking the old one as deleted.

**Key Architecture Points**:

```c
TM_Result heap_update(Relation relation, ItemPointer otid, HeapTuple newtup,
                     CommandId cid, Snapshot crosscheck, bool wait,
                     TM_FailureData *tmfd, LockTupleMode *lockmode,
                     TU_UpdateIndexes *update_indexes)
{
    TransactionId xid = GetCurrentTransactionId();
    
    // 1. Determine which attributes are being modified
    hot_attrs = RelationGetIndexAttrBitmap(relation, INDEX_ATTR_BITMAP_HOT);
    key_attrs = RelationGetIndexAttrBitmap(relation, INDEX_ATTR_BITMAP_KEY);
    id_attrs = RelationGetIndexAttrBitmap(relation, INDEX_ATTR_BITMAP_IDENTITY_KEY);
    
    // 2. Read old tuple and check visibility
    block = ItemPointerGetBlockNumber(otid);
    buffer = ReadBuffer(relation, block);
    // ... visibility and locking logic similar to heap_delete
    
    // 3. Prepare new tuple
    heaptup = heap_prepare_insert(relation, newtup, xid, cid, options);
    
    // 4. Determine update strategy (HOT vs. non-HOT)
    // HOT (Heap-Only Tuple) updates possible if:
    // - No indexed attributes changed
    // - Sufficient space on same page
    
    if (/* conditions for HOT update */) {
        use_hot_update = true;
        newbuf = buffer;  // Same page
    } else {
        // Find new page for updated tuple
        newbuf = RelationGetBufferForTuple(relation, heaptup->t_len, ...);
    }
    
    // 5. Critical section for atomic update
    START_CRIT_SECTION();
    
    // Mark old tuple as updated
    HeapTupleHeaderSetXmax(oldtup.t_data, xid);
    
    // Place new tuple
    RelationPutHeapTuple(relation, newbuf, heaptup, false);
    
    // Set up tuple chain (ctid pointer from old to new)
    HeapTupleHeaderSetCtid(oldtup.t_data, &heaptup->t_self);
    
    // Handle visibility maps, WAL logging, etc.
    
    END_CRIT_SECTION();
    
    return TM_Ok;
}
```

## Table Access Method Integration

The heap access method integrates with PostgreSQL's pluggable table access method system through `heapam_handler.c`. Key integration points:

### Handler Registration

```c
static const TableAmRoutine heapam_methods = {
    .type = T_TableAmRoutine,
    
    // Scanning functions
    .scan_begin = heap_beginscan,
    .scan_getnextslot = heap_getnextslot,
    .scan_end = heap_endscan,
    
    // DML functions  
    .tuple_insert = heapam_tuple_insert,
    .tuple_delete = heapam_tuple_delete,
    .tuple_update = heapam_tuple_update,
    
    // ... other methods
};
```

### Wrapper Functions

The handler provides wrapper functions that adapt between the table AM interface and heap-specific implementations:

```c
// Insert wrapper - converts TupleTableSlot to HeapTuple
static void heapam_tuple_insert(Relation relation, TupleTableSlot *slot, ...)
{
    HeapTuple tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);
    heap_insert(relation, tuple, cid, options, bistate);
    ItemPointerCopy(&tuple->t_self, &slot->tts_tid);
}

// Delete wrapper - direct passthrough
static TM_Result heapam_tuple_delete(Relation relation, ItemPointer tid, ...)
{
    return heap_delete(relation, tid, cid, crosscheck, wait, tmfd, changingPart);
}

// Update wrapper - converts TupleTableSlot to HeapTuple  
static TM_Result heapam_tuple_update(Relation relation, ItemPointer otid, 
                                   TupleTableSlot *slot, ...)
{
    HeapTuple tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);
    result = heap_update(relation, otid, tuple, cid, crosscheck, wait, ...);
    ItemPointerCopy(&tuple->t_self, &slot->tts_tid);
    return result;
}
```

## Key Design Patterns for ORF Implementation

Based on this analysis, implementing DML operations for the optimized_row_format extension should follow these patterns:

### 1. MVCC Compliance
- Never modify tuples in place
- Use transaction IDs (xmin/xmax) for visibility control
- Implement proper visibility checking functions
- Handle hint bits for performance optimization

### 2. Buffer Management
- Use PostgreSQL's shared buffer pool
- Acquire appropriate locks (shared for reads, exclusive for writes)
- Mark buffers dirty after modifications
- Handle visibility map updates

### 3. WAL Integration
- Log all modifications for durability and replication
- Use critical sections around atomic operations
- Follow existing WAL record formats where possible

### 4. Page-Level Operations
- Respect page layout and space management
- Handle tuple placement and fragmentation
- Implement proper page initialization and cleanup

### 5. Table AM Integration
- Provide wrapper functions that adapt to table AM interface
- Handle TupleTableSlot to/from internal tuple format conversions
- Maintain compatibility with executor expectations

### 6. Error Handling and Concurrency
- Handle concurrent modifications gracefully
- Implement proper retry and wait logic
- Use appropriate isolation levels and conflict detection

## Implementation Recommendations

For the optimized_row_format extension:

1. **Start with scanning functionality** - Implement orf_getnext() based on heap_getnext patterns
2. **Implement insert operations** - Focus on tuple preparation and page placement
3. **Add delete/update support** - Leverage MVCC principles from heap implementation
4. **Optimize for ORF-specific features** - Take advantage of optimized storage format while maintaining compatibility
5. **Test thoroughly** - Ensure MVCC semantics are preserved and concurrency works correctly

The heap access method provides a solid foundation and reference implementation for building custom table access methods in PostgreSQL.
