# Table Scan Implementation for Table Access Methods

A core responsibility of a custom Table Access Method (AM) is to implement sequential scans. This is the primary way PostgreSQL reads data for `SELECT` queries, and it's also used by other operations like `VACUUM` and `ANALYZE`. The three key functions that a Table AM must provide for this are `scan_begin`, `scan_getnextslot`, and `scan_end`.

## 1. The Table Scan Workflow

When the executor initiates a table scan, it calls these functions in a specific order:
1.  **`scan_begin`**: Called once at the start of the scan to set up the necessary state.
2.  **`scan_getnextslot`**: Called repeatedly. Each call should return the next visible tuple in the table. It returns `false` when all tuples have been scanned.
3.  **`scan_end`**: Called once at the end of the scan to release all resources.

## 2. `scan_begin`

This function's job is to initialize everything needed for the scan. A robust pattern is to define a custom scan descriptor struct that embeds the standard `TableScanDescData` at its core. This custom struct holds the scan's state, such as the current position (block and offset) and the buffer containing the current page.

### Skeleton Code: `scan_begin`
```c
// In my_am_handler.c

// Custom scan descriptor for our AM
typedef struct MyScanDescData
{
    TableScanDescData rs_base;  // Must be the first field

    // AM-specific state for the scan
    Relation rel;
    BlockNumber nblocks;
    BlockNumber currBlock;
    OffsetNumber currOffset;
    Buffer currBuffer;
} MyScanDescData;

typedef MyScanDescData *MyScanDesc;

static TableScanDesc
my_am_scan_begin(Relation rel, Snapshot snapshot,
                 int nkeys, struct ScanKeyData *key,
                 ParallelTableScanDesc pscan, uint32 flags)
{
    MyScanDesc scan = (MyScanDesc) palloc0(sizeof(MyScanDescData));

    // Initialize the standard part of the scan descriptor
    scan->rs_base.rs_rd = rel;
    scan->rs_base.rs_snapshot = snapshot;
    scan->rs_base.rs_nkeys = nkeys;
    scan->rs_base.rs_key = key;
    // ... other standard fields ...

    // Initialize our custom state
    scan->rel = rel;
    scan->nblocks = RelationGetNumberOfBlocks(rel);
    scan->currBlock = 0;
    scan->currOffset = FirstOffsetNumber;
    scan->currBuffer = InvalidBuffer; // No buffer held initially

    return &scan->rs_base;
}
```

## 3. `scan_getnextslot`

This is the most complex of the three functions. It contains the main loop for iterating through tuples and pages.

### Workflow:
1.  **Outer Loop (Pages)**: Loop from the current block (`currBlock`) up to the total number of blocks (`nblocks`).
2.  **Load Buffer**: If no buffer is currently held, read the current block into a buffer and acquire a share lock.
3.  **Inner Loop (Tuples)**: Loop through all the item pointers on the current page, from the current offset (`currOffset`).
4.  **Fetch and Check Visibility**: For each valid item, get the tuple header and check if it's visible to the scan's snapshot (`HeapTupleSatisfiesVisibility`).
5.  **Load Slot**: If the tuple is visible, load it into the provided `TupleTableSlot`. The most efficient way to do this for an AM that supports projection is to:
    a. Store the raw tuple data using `ExecStoreHeapTuple`.
    b. Set `slot->tts_nvalid = 0;` to force the executor to use the AM's custom `slot_callbacks` for on-demand attribute extraction.
6.  **Return**: If a visible tuple is found and loaded, return `true`.
7.  **Advance and Clean Up**: If the inner loop finishes, release the lock on the current buffer, advance to the next block, and reset the offset.
8.  **End of Scan**: If the outer loop finishes, it means all tuples have been scanned. Return `false`.

### Skeleton Code: `scan_getnextslot`
```c
// In my_am_handler.c

static bool
my_am_scan_getnextslot(TableScanDesc scan, ScanDirection direction,
                       TupleTableSlot *slot)
{
    MyScanDesc an_scan = (MyScanDesc) scan;
    HeapTupleData tuple; // A temporary HeapTupleData to wrap our tuple

    while (an_scan->currBlock < an_scan->nblocks)
    {
        if (!BufferIsValid(an_scan->currBuffer))
        {
            an_scan->currBuffer = ReadBuffer(an_scan->rel, an_scan->currBlock);
            LockBuffer(an_scan->currBuffer, BUFFER_LOCK_SHARE);
        }

        Page page = BufferGetPage(an_scan->currBuffer);
        OffsetNumber maxOffset = PageGetMaxOffsetNumber(page);

        while (an_scan->currOffset <= maxOffset)
        {
            ItemId itemid = PageGetItemId(page, an_scan->currOffset);
            if (ItemIdIsUsed(itemid))
            {
                tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
                tuple.t_len = ItemIdGetLength(itemid);
                ItemPointerSet(&tuple.t_self, an_scan->currBlock, an_scan->currOffset);

                if (HeapTupleSatisfiesVisibility(&tuple, scan->rs_snapshot, an_scan->currBuffer))
                {
                    ExecStoreHeapTuple(&tuple, slot, false); // Store raw tuple
                    slot->tts_nvalid = 0; // Force projection via slot_callbacks

                    an_scan->currOffset++;
                    UnlockBuffer(an_scan->currBuffer); // Unlock but keep pin
                    return true;
                }
            }
            an_scan->currOffset++;
        }

        // Finished with this block, release the buffer and move to the next
        UnlockReleaseBuffer(an_scan->currBuffer);
        an_scan->currBuffer = InvalidBuffer;
        an_scan->currBlock++;
        an_scan->currOffset = FirstOffsetNumber;
    }

    return false; // No more tuples
}
```

## 4. `scan_end`

This is the simplest function. It's called at the end of the scan to release any resources held by the scan descriptor.

### Skeleton Code: `scan_end`
```c
// In my_am_handler.c

static void
my_am_scan_end(TableScanDesc scan)
{
    MyScanDesc an_scan = (MyScanDesc) scan;

    // Release the buffer pin if the scan is ending mid-page
    if (BufferIsValid(an_scan->currBuffer))
    {
        ReleaseBuffer(an_scan->currBuffer);
    }

    // Free the memory for our custom scan descriptor
    pfree(an_scan);
}
``` 