
# Optimized Row Format: C-Level Implementation Details

This document provides a technical deep-dive into the current C-level implementation of the `optimized_row_format` Table Access Method (AM). It is intended for developers working on the module and assumes familiarity with PostgreSQL's AM interface and tuple internals.

## 1. Overview of Key AM Functions

The `optimized_row_format` AM is registered via the `optimized_row_format_tableam_handler` function. It copies the standard `heap` AM routine table and overrides several key functions to implement its custom logic.

### `optimized_tuple_insert`

**Current Implementation:**
The `optimized_tuple_insert` function currently delegates the entire insertion process to the standard heap AM's `tuple_insert` function.

```c
static void
optimized_tuple_insert(Relation relation, TupleTableSlot *slot,
                      CommandId cid, int options, struct BulkInsertStateData *bistate)
{
    const TableAmRoutine *heap_am = get_heap_am_routine();
    heap_am->tuple_insert(relation, slot, cid, options, bistate);
}
```

**Implication:**
The custom on-disk format described in the main `README.md` is **not yet implemented**. Tuples are currently written in the standard heap format, not the optimized format. This was a deliberate choice to simplify initial development and testing of the scan and projection mechanisms.

### `optimized_scan_begin`

This function initializes a table scan. It allocates a custom scan descriptor `OptimizedScanDescData` which embeds the standard `TableScanDescData`.

**Current Implementation:**
It performs standard scan setup and initializes its internal state, such as setting the current block and offset numbers. It does not yet leverage any custom data structures for the scan itself.

### `optimized_scan_getnextslot`

This is the core of the table scan. It fetches the next visible tuple and places it into the provided `TupleTableSlot`.

**Current Implementation:**
The function iterates through pages and items (tuples) on each page, checking for visibility using standard heap functions (`heap_page_prune`, `HeapTupleSatisfiesMVCC`). When a visible tuple is found, it is stored in the slot using `ExecStoreHeapTuple`.

**Key Observation:** It uses the standard heap functions to navigate the page, reinforcing that the on-disk format is currently just the standard heap format.

## 2. TupleTableSlot Handling and the `INSERT` Bug

A critical bug was discovered that caused `INSERT` operations to fail with the error: `ERROR: trying to store a heap tuple into wrong type of slot`.

### Root Cause Analysis

1.  **Custom Slot Callbacks:** The AM's `slot_callbacks` function (`optimized_slot_callbacks`) was returning a custom `TupleTableSlotOps` struct named `TTSOpsOptimized`.
2.  **Purpose of `TTSOpsOptimized`:** This custom struct was designed to enable projection optimizations during scans by overriding the `getsomeattrs` function pointer. This would allow the AM to de-form only the columns requested by a query, rather than all columns.
3.  **The Conflict with `INSERT`:** The PostgreSQL executor's `INSERT` path creates a standard `HeapTuple` from the `VALUES` clause. It then uses the `ExecStoreHeapTuple` function to place this tuple into a slot provided by the table AM. This function has a hard-coded check that requires the slot's `tts_ops` to be either `&TTSOpsHeapTuple` or `&TTSOpsBufferHeapTuple`.
4.  **The Error:** Because `optimized_slot_callbacks` returned `&TTSOpsOptimized`, the check in `ExecStoreHeapTuple` failed, resulting in the error. The AM was providing a slot optimized for its own scanning needs, which was incompatible with the executor's general insertion pathway.

### The Fix

The immediate fix was to make `optimized_slot_callbacks` return `&TTSOpsHeapTuple`, making it compatible with the executor's requirements for insertion.

```c
static const TupleTableSlotOps *
optimized_slot_callbacks(Relation relation)
{
    /* This now returns the standard heap slot ops to allow INSERTs to work. */
    return &TTSOpsHeapTuple;
}
```

## 3. Known Issues and Next Steps

### Projection Optimization is Disabled

The primary consequence of the `slot_callbacks` fix is that the intended projection optimization (via a custom `getsomeattrs`) is **currently disabled**. All scans will de-form all attributes of a tuple, regardless of which columns are requested by the query. This negates a key performance benefit of the custom AM.

### Recommended Next Step: Re-enabling Projection

The projection logic needs to be re-enabled without breaking `INSERT`. A promising approach is to **dynamically switch the slot's `tts_ops`**.

1.  The `optimized_slot_callbacks` function should continue to return `&TTSOpsHeapTuple` to ensure compatibility with `INSERT`.
2.  Inside `optimized_scan_getnextslot`, *after* a tuple has been successfully fetched and stored in the slot (which now has `TTSOpsHeapTuple`), the implementation should manually switch the slot's operations to the custom `&TTSOpsOptimized`.

**Proposed Implementation:**

```c
static bool
optimized_scan_getnextslot(TableScanDesc scan, ScanDirection direction,
                          TupleTableSlot *slot)
{
    // ... existing logic to find and fetch a visible tuple ...

    if (tuple_found)
    {
        // Store the tuple using the standard heap slot ops. This works.
        ExecStoreHeapTuple(heapTuple, slot, true);

        // NOW, switch the slot's ops to our custom ones for this scan.
        // The executor will use these ops for any subsequent getattr/getsomeattrs
        // calls for this specific tuple.
        slot->tts_ops = &TTSOpsOptimized; // Defined elsewhere in the file

        return true;
    }

    return false;
}
```

This approach satisfies the requirements of both the `INSERT` path (which gets a standard heap slot) and the `SELECT` path (which gets a slot with optimized projection capabilities after the tuple is loaded). This is the most critical next step for development. 