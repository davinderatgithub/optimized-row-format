# Optimized Row Format Extension - Next Steps Action Plan

Congratulations on fixing the `COUNT(*)` issue! That confirms the core insertion and tuple visibility logic is working.

Based on a review of the latest test results (`many_columns_results_...`), the next priority is to address the severe performance regressions and implement missing core functionalities. The current scan implementation, while returning rows, is thousands of times slower than the standard heap.

This updated plan prioritizes making the extension performant and then robust.

## Priority 1: Fix Catastrophic Scan Performance (URGENT)

### Root Cause
The current `optimized_scan_getnextslot` function is deforming the *entire tuple* for every row it scans, regardless of how many columns the query actually needs. This eager, full-tuple decompression is why `SELECT` performance is extremely poor in the many-columns test (e.g., 24 seconds vs. 5 milliseconds).

### Immediate Actions

#### Step 1: Implement On-Demand Attribute Fetching (Projection)
The architecture must be changed from "deform all columns now" to "deform one column on demand".

1.  **Create Custom TupleTableSlotOps:**
    In `optimized_row_format.c`, define a new `TupleTableSlotOps` struct (e.g., `TTSOpsOptimizedTuple`). This will be a copy of `TTSOpsHeapTuple` but with its `.getattr` field pointing to our existing `optimized_getattr` function.

2.  **Refactor `optimized_scan_getnextslot`:**
    -   Find the next visible tuple and wrap it in a `HeapTupleData` struct as is currently done.
    -   **Remove the `for` loop that iterates through all attributes.**
    -   Instead, use `ExecStoreHeapTuple(tuple, slot, true)` to store the raw tuple pointer in the slot.
    -   After storing the tuple, **override the slot's operations**: `slot->tts_ops = &TTSOpsOptimizedTuple;`. This is the critical step that directs the database executor to use our `optimized_getattr` for subsequent on-demand attribute fetching.

### Success Metric
-   Single-column `SELECT` queries on the many-columns test should be significantly faster than the standard heap, not slower.

---

## Priority 2: Implement Core DML Operations

### Root Cause
The extension currently delegates `UPDATE` and `DELETE` operations to the heap access method, which will fail or corrupt data because it doesn't understand the optimized format.

### Action Plan

1.  **Implement `optimized_tuple_delete`:**
    -   This function will find the target tuple using its `ctid`.
    -   It will then mark the tuple as deleted by setting its `t_xmax` to the current transaction ID. This is standard MVCC.

2.  **Implement `optimized_tuple_update`:**
    -   This is a "delete" then "insert" operation.
    -   Mark the old tuple version as deleted by setting its `t_xmax` and updating its `t_ctid` to point to the new tuple's location.
    -   Use the existing `optimized_tuple_insert` logic to insert the new version of the tuple.

3.  **Create Correctness Tests:**
    -   Add `UPDATE` and `DELETE` operations to `test/sql/correctness.sql` and verify the results are identical to the heap implementation.

---

## Priority 3: Address Indexing and Known Bugs

### Root Cause
The `README.md` notes that creating a `PRIMARY KEY` fails, and `NULL` values were causing crashes. These issues block wider functionality.

### Action Plan

1.  **Enable Primary Key / Index Support:**
    -   The `ERROR: only heap AM is supported` indicates that index-related functions are missing.
    -   Implement the required index support functions in the `TableAmRoutine`:
        -   `index_fetch_begin`
        -   `index_fetch_tuple` (This is the most important one to start with. It must take an `ItemPointer` from the index and return the matching tuple from the table's page).
        -   `index_fetch_end`
    -   Add a test case to `correctness.sql` that creates a primary key and queries via the index.

2.  **Fix NULL Value Crash:**
    -   Add test cases with various `NULL` patterns to `correctness.sql`.
    -   Use a debugger or extensive logging in `optimized_tuple_insert` and `optimized_getattr` to trace the handling of the null bitmap and data pointers to find the source of the invalid memory access.

---

## Priority 4: Address Performance and Storage Regressions

### Root Cause
Test results show that `INSERT` operations are slower and storage consumption is higher than the standard heap.

### Action Plan

1.  **Investigate INSERT Performance:**
    -   Once the scan performance is fixed, profile `optimized_tuple_insert`.
    -   The overhead likely comes from the two-pass approach (calculating sizes then copying) or from building the variable-offset array. Investigate if this can be done in a single pass.

2.  **Investigate Storage Inefficiency:**
    -   The larger table size is counter-intuitive.
    -   Add detailed logging to the tuple length calculation in `optimized_tuple_insert`.
    -   Log the size of each component (header, null bitmap, offset array, fixed data, var data) and the effects of `MAXALIGN`. Compare this byte-for-byte with a standard heap tuple for the same row to find the source of the bloat.

## Implementation Timeline

### Week 1: Critical Performance Fix
- [ ] Implement custom `TupleTableSlotOps` for on-demand `getattr`.
- [ ] Refactor `optimized_scan_getnextslot` to remove the eager deform loop.
- [ ] **Verify:** `SELECT` queries on the many-columns test are now faster than heap.

### Week 2-3: Core Functionality
- [ ] Implement `optimized_tuple_delete`.
- [ ] Implement `optimized_tuple_update`.
- [ ] Add `UPDATE`/`DELETE` tests to `correctness.sql`.
- [ ] Implement basic `index_fetch_tuple` to allow primary key creation.

### Month 2: Bug Fixing and Optimization
- [ ] Create and pass tests for `NULL` value handling.
- [ ] Profile and optimize `INSERT` performance.
- [ ] Analyze and fix the storage inefficiency.
- [ ] Begin implementing production-readiness features like `VACUUM` and WAL logging.