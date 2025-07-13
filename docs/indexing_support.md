# Indexing Support for Table Access Methods

To integrate a custom table access method with PostgreSQL's indexing system, it must provide a set of callback functions. These functions are responsible for bridging the gap between an index scan (which provides `TID`s, or tuple identifiers) and the table storage (which must retrieve the actual tuples). The primary functions are `index_fetch_begin`, `index_fetch_tuple`, and `index_fetch_end`.

## 1. The Index Scan Workflow

When the executor performs an index scan, it first consults the index to find the `TID`s of tuples that match the query's conditions. For each `TID` returned by the index, the executor calls the table AM's `index_fetch_tuple` function to retrieve the actual tuple from the table's storage. This process is framed by `index_fetch_begin` and `index_fetch_end`, which manage the state of the scan.

## 2. `index_fetch_begin`

This function is called once at the start of an index scan. Its primary responsibility is to allocate and initialize a state structure that will be used throughout the scan. This state is passed to subsequent calls to `index_fetch_tuple`.

A common pattern is to create a custom struct that embeds the standard `IndexFetchTableData` and includes any additional state the AM needs, such as a reference to the currently held buffer.

### Skeleton Code: `index_fetch_begin`
```c
// In my_am_handler.c

typedef struct MyIndexFetchData
{
    IndexFetchTableData xs_base; // Base struct must be first
    Buffer      xs_cbuf;         // Cached buffer handle
    // Add any other AM-specific state here
} MyIndexFetchData;

static IndexFetchTableData *
my_am_index_fetch_begin(Relation rel)
{
    MyIndexFetchData *scan_data = palloc0(sizeof(MyIndexFetchData));

    scan_data->xs_base.rel = rel;
    scan_data->xs_cbuf = InvalidBuffer; // Initialize buffer to invalid

    return &scan_data->xs_base;
}
```

## 3. `index_fetch_tuple`

This is the workhorse of the index fetch mechanism. It's called for each `TID` found in the index. Its job is to locate the tuple on the correct page, perform visibility checks, and load the visible tuple into the provided `TupleTableSlot`.

The standard `heapam` implementation includes an important optimization: it keeps the last-used buffer pinned and only switches buffers when the `TID` points to a different block. It also handles Heap-Only Tuple (HOT) chains by using `heap_hot_search_buffer` to find the current, visible version of a row.

A custom AM must implement similar logic to translate a `TID` into a physical tuple and check its visibility against the current `snapshot`.

### Skeleton Code: `index_fetch_tuple`
```c
// In my_am_handler.c

static bool
my_am_index_fetch_tuple(struct IndexFetchTableData *scan,
                         ItemPointer tid,
                         Snapshot snapshot,
                         TupleTableSlot *slot,
                         bool *call_again, bool *all_dead)
{
    MyIndexFetchData *scan_data = (MyIndexFetchData *) scan;
    bool tuple_found = false;

    // 1. Check if we need to switch buffers. If the block number in 'tid'
    //    is different from the one in scan_data->xs_cbuf, release the old
    //    buffer (if valid) and read the new one.
    //
    //    Buffer buf = ReleaseAndReadBuffer(scan_data->xs_cbuf, ...);
    //    scan_data->xs_cbuf = buf;

    // 2. Lock the buffer for shared access.
    //    LockBuffer(scan_data->xs_cbuf, BUFFER_LOCK_SHARE);

    // 3. Find the tuple on the page using the offset from 'tid'.
    //    Page page = BufferGetPage(scan_data->xs_cbuf);
    //    OffsetNumber offset = ItemPointerGetOffsetNumber(tid);
    //    ItemId itemid = PageGetItemId(page, offset);
    //    ...

    // 4. Perform visibility check against the snapshot. This is a complex
    //    part. You need to replicate the logic of HeapTupleSatisfiesVisibility
    //    or a similar function suitable for your AM's tuple format.
    //
    //    if (MyAmTupleSatisfiesVisibility(..., snapshot))
    //    {
    //        // 5. If visible, deform the tuple from your custom format
    //        //    and store it in the provided slot.
    //
    //        my_am_deform_tuple_into_slot(itemid, slot);
    //        ExecStoreBufferHeapTuple(..., slot, scan_data->xs_cbuf); // Example
    //        tuple_found = true;
    //    }

    // 6. Unlock the buffer.
    //    LockBuffer(scan_data->xs_cbuf, BUFFER_LOCK_UNLOCK);

    // 'call_again' is for non-MVCC snapshots. Usually, it can be set to false.
    *call_again = false;
    // 'all_dead' can be set to true if you are certain no transaction will
    // ever see this tuple, allowing the index to be cleaned up.
    if (all_dead)
        *all_dead = false; // Be conservative unless sure.

    return tuple_found;
}
```

## 4. `index_fetch_end`

This function is called at the end of the scan to release any resources allocated by `index_fetch_begin`. Its main job is to release the buffer pin, if one is still held, and free the state structure.

### Skeleton Code: `index_fetch_end`
```c
// In my_am_handler.c

static void
my_am_index_fetch_end(IndexFetchTableData *scan)
{
    MyIndexFetchData *scan_data = (MyIndexFetchData *) scan;

    // Release the last buffer, if it's valid
    if (BufferIsValid(scan_data->xs_cbuf))
    {
        ReleaseBuffer(scan_data->xs_cbuf);
    }

    pfree(scan_data);
}
``` 