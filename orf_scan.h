#ifndef ORF_SCAN_H
#define ORF_SCAN_H

#include "postgres.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/tableam.h"

/* Scan-related function declarations */
TableScanDesc optimized_scan_begin(Relation rel, Snapshot snapshot,
                                   int nkeys, struct ScanKeyData *key,
                                   ParallelTableScanDesc pscan, uint32 flags);
void optimized_scan_end(TableScanDesc scan);
void optimized_scan_rescan(TableScanDesc scan, ScanKey key, bool set_params,
                           bool allow_strat, bool allow_sync, bool allow_pagemode);
bool optimized_scan_getnextslot(TableScanDesc scan, ScanDirection direction,
                                TupleTableSlot *slot);
Size optimized_parallelscan_estimate(Relation rel);

/* Index Scan Functions */
IndexFetchTableData *optimized_index_fetch_begin(Relation rel);
void optimized_index_fetch_reset(IndexFetchTableData *scan);
void optimized_index_fetch_end(IndexFetchTableData *scan);
bool optimized_index_fetch_tuple(struct IndexFetchTableData *scan,
                                 ItemPointer tid,
                                 Snapshot snapshot,
                                 TupleTableSlot *slot,
                                 bool *call_again, bool *all_dead);

#endif /* ORF_SCAN_H */
