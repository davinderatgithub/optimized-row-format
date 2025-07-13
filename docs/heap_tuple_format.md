# Standard PostgreSQL Heap Tuple Format

This document outlines the on-disk structure of a standard PostgreSQL heap tuple and the rules governing its visibility according to the Multi-Version Concurrency Control (MVCC) model.

## 1. On-Disk Tuple Structure

A standard heap tuple is composed of a header (`HeapTupleHeaderData`) followed by user data. The layout is as follows:

1.  **Fixed Header (`HeapTupleHeaderData`)**: Contains transaction identifiers (`xmin`, `xmax`), command identifiers (`cmin`, `cmax`), and various flag bits (`t_infomask`, `t_infomask2`) that describe the tuple's state.
2.  **Null Bitmap**: An optional bitmap that indicates which columns are NULL. It is only present if the `HEAP_HASNULL` flag is set in `t_infomask`.
3.  **User Data**: The actual data for the tuple's columns, aligned as required by their data types.

### `HeapTupleHeaderData`

This structure is defined in `src/include/access/htup_details.h` and is the core of the MVCC mechanism. Key fields include:

-   `t_xmin`: The transaction ID (XID) of the transaction that inserted this tuple.
-   `t_xmax`: The XID of the transaction that deleted or locked this tuple. If 0, the tuple is not deleted or locked.
-   `t_cid`: The command ID (CID) within the transaction.
-   `t_ctid`: A pointer to the current or newer version of this tuple. For an updated row, this points to the new tuple.
-   `t_infomask` & `t_infomask2`: Bitmasks containing crucial information about the tuple's state.

### `t_infomask` Flags

The `t_infomask` field contains flags that are critical for visibility checks:

-   `HEAP_XMIN_COMMITTED`: Set if `t_xmin` is known to have committed.
-   `HEAP_XMIN_INVALID`: Set if `t_xmin` is known to have aborted.
-   `HEAP_XMAX_COMMITTED`: Set if `t_xmax` is known to have committed.
-   `HEAP_XMAX_INVALID`: Set if `t_xmax` is known to have aborted.
-   `HEAP_XMAX_IS_MULTI`: Set if `t_xmax` is a `MultiXactId` (i.e., locked by multiple transactions).
-   `HEAP_XMAX_LOCK_ONLY`: Set if `t_xmax` represents a lock, not a deletion.

## 2. MVCC Visibility Rules

The function `HeapTupleSatisfiesMVCC` (in `src/backend/access/heap/heapam_visibility.c`) determines if a tuple is visible to a given transaction snapshot. The core logic is as follows:

A tuple is **visible** if:

1.  **The inserting transaction committed and is visible to our snapshot:**
    -   `t_xmin` is valid, committed (`HEAP_XMIN_COMMITTED` is set or `TransactionIdDidCommit` is true), and its XID is "in the past" relative to our snapshot's `xmin`.
    -   **AND**

2.  **The deleting transaction is not visible to our snapshot:**
    -   `t_xmax` is invalid (`HEAP_XMAX_INVALID` is set or the transaction aborted).
    -   **OR** `t_xmax` is valid, but the transaction is still in progress relative to our snapshot (`XidInMVCCSnapshot` is true).
    -   **OR** `t_xmax` represents a lock only (`HEAP_XMAX_LOCK_ONLY` is set).

A tuple is **not visible** if:

1.  **The inserting transaction aborted:**
    -   `t_xmin` is invalid (`HEAP_XMIN_INVALID` is set).

2.  **The inserting transaction is still in progress:**
    -   `t_xmin` is a running transaction according to our snapshot (`XidInMVCCSnapshot` is true).

3.  **The inserting transaction was performed by our own transaction but after our snapshot was taken:**
    -   `t_xmin` is our own XID, but its `cmin` is greater than or equal to our snapshot's `curcid`.

4.  **The deleting transaction committed and is visible to our snapshot:**
    -   `t_xmax` is valid, committed, not just a lock, and its XID is "in the past" relative to our snapshot.

This logic ensures that each transaction sees a consistent "snapshot" of the database, as if it were the only transaction running. The combination of `xmin`, `xmax`, and the `t_infomask` hint bits allows for efficient visibility checks without constantly querying the transaction log. 