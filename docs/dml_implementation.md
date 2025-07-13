# DML Implementation for Table Access Methods

Implementing Data Manipulation Language (DML) operations (`UPDATE`, `DELETE`) in a custom Table Access Method (AM) requires careful handling of PostgreSQL's Multi-Version Concurrency Control (MVCC) model. Unlike traditional databases, PostgreSQL never modifies tuple data in-place. Instead, it creates new versions of tuples and controls their visibility to different transactions.

This guide explains the core concepts and provides skeleton code for implementing `tuple_delete` and `tuple_update`.

## 1. The MVCC Model in DML

The fundamental principle is that `DELETE` and `UPDATE` operations do not physically remove or alter old data.
- A **`DELETE`** operation marks a tuple as no longer valid from the current transaction onwards.
- An **`UPDATE`** operation is essentially a `DELETE` of the old tuple followed by an `INSERT` of a new version of the tuple.

This is primarily managed by two fields in the tuple header (`HeapTupleHeaderData`): `t_xmin` and `t_xmax`.
- `t_xmin`: The transaction ID (XID) that inserted the tuple. The tuple is only visible to transactions that started *after* this XID.
- `t_xmax`: The XID that deleted or updated the tuple. If this is set, the tuple is considered invalid for any transaction that started *after* this XID.

## 2. Implementing `tuple_delete`

The `tuple_delete` function is responsible for marking a tuple as deleted.

### Workflow:
1.  **Find and Lock the Tuple**: Locate the tuple on its page using the provided `ItemPointer` (TID). Acquire a lock on the tuple to prevent concurrent modifications.
2.  **Check Visibility**: Verify that the tuple is visible to the current transaction's snapshot. Another transaction might have already deleted or updated it. `HeapTupleSatisfiesUpdate` is the standard function for this check.
3.  **Mark as Deleted**: If the visibility check passes, set the tuple's `t_xmax` field to the current transaction's XID. This logically "deletes" the tuple.
4.  **Update Hint Bits**: Set the appropriate hint bits in the tuple's `infomask` to signal that it has been updated. This allows other transactions to determine its status more quickly.
5.  **Write to WAL**: Log the change to the Write-Ahead Log (WAL) to ensure durability and for replication purposes.
6.  **Mark Page as Dirty**: Mark the buffer containing the page as dirty so the changes are written to disk.

### Skeleton Code: `tuple_delete`
```c
// In my_am_handler.c

static TM_Result
my_am_tuple_delete(Relation relation, ItemPointer tid, CommandId cid,
                   Snapshot snapshot, Snapshot crosscheck, bool wait,
                   TM_FailureData *tmfd, bool changingPart)
{
    // 1. Read the buffer containing the tuple page
    Buffer buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
    Page page = BufferGetPage(buffer);
    TM_Result result;

    // 2. Lock the buffer and the tuple
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    // ... logic to acquire tuple lock ...

    // 3. Get the tuple header
    MyAmTupleHeader tuple_header = (MyAmTupleHeader) PageGetItem(page, PageGetItemId(page, ItemPointerGetOffsetNumber(tid)));

    // 4. Check tuple visibility using a function similar to HeapTupleSatisfiesUpdate
    // This is a complex check involving the snapshot and tuple's xmin/xmax.
    result = MyAmTupleSatisfiesUpdate(tuple_header, snapshot);

    if (result == TM_Ok)
    {
        // 5. Mark the tuple as deleted by setting t_xmax
        tuple_header->t_xmax = GetCurrentTransactionId();
        // Also set infomask bits, e.g., HEAP_XMAX_INVALID

        // Mark the buffer as dirty
        MarkBufferDirty(buffer);

        // 6. Write to WAL (simplified representation)
        // log_heap_delete(...);
    }

    // 7. Unlock and release resources
    LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
    ReleaseBuffer(buffer);

    return result;
}
```

## 3. Implementing `tuple_update`

An `UPDATE` is a more complex operation that combines a `DELETE` with an `INSERT`.

### Workflow:
1.  **Find and Lock Old Tuple**: Just like in `delete`, find and lock the old version of the tuple.
2.  **Check Visibility**: Verify the old tuple is visible to the current snapshot.
3.  **Mark Old Tuple as Updated**:
    - Set the `t_xmax` of the old tuple to the current XID.
    - **Crucially, set the `t_ctid` field of the old tuple to point to the TID of the *new* tuple that will be inserted.** This creates the version chain.
4.  **Insert New Tuple**: Call the AM's `tuple_insert` function to add the new version of the tuple to the table. The new tuple will have its `t_xmin` set to the current XID, making it the currently valid version.
5.  **Handle HOT Updates**: If possible (i.e., no indexed columns were changed), try to place the new tuple on the same page as the old one. This is a Heap-Only Tuple (HOT) update and avoids expensive index updates.
6.  **Write to WAL**: Log both the update to the old tuple and the insertion of the new tuple.

### Skeleton Code: `tuple_update`
```c
// In my_am_handler.c

static TM_Result
my_am_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
                   CommandId cid, Snapshot snapshot, Snapshot crosscheck,
                   bool wait, TM_FailureData *tmfd,
                   LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
    // ... (Steps 1 & 2: Find, lock, and check visibility of the old tuple at 'otid') ...
    // Assume we have the old tuple header in 'old_tuple_header' and the buffer locked.

    TM_Result result = MyAmTupleSatisfiesUpdate(old_tuple_header, snapshot);

    if (result == TM_Ok)
    {
        // Try to perform a HOT update if possible
        bool can_hot_update = CheckForHOTUpdate(...);

        // 3. Insert the new tuple (from the 'slot')
        // This will return the TID of the newly inserted tuple.
        ItemPointerData new_tid;
        my_am_tuple_insert_for_update(relation, slot, &new_tid, can_hot_update);

        // 4. Mark the old tuple as updated and link it to the new one
        old_tuple_header->t_xmax = GetCurrentTransactionId();
        old_tuple_header->t_ctid = new_tid; // Link to the new version

        // Mark buffer dirty for old tuple
        MarkBufferDirty(old_buffer);

        // ... (WAL logging for both insert and update) ...
    }

    // ... (Unlock and release resources) ...

    return result;
}
``` 