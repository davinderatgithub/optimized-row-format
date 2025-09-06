/*
 * orf_functions.h
 *
 * Function declarations for optimized_row_format extension
 */

#ifndef ORF_FUNCTIONS_H
#define ORF_FUNCTIONS_H

#include "postgres.h"
#include "access/tableam.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "executor/tuptable.h"
#include "storage/bufmgr.h"
#include "executor/executor.h"

/* Scan functions */
extern TableScanDesc optimized_scan_begin(Relation relation,
                                         Snapshot snapshot,
                                         int nkeys,
                                         ScanKey key,
                                         ParallelTableScanDesc parallel_scan,
                                         uint32 flags);

extern void optimized_scan_end(TableScanDesc scan);

extern void optimized_scan_rescan(TableScanDesc scan,
                                 ScanKey key,
                                 bool set_params,
                                 bool allow_strat,
                                 bool allow_sync,
                                 bool allow_pagemode);

extern bool optimized_scan_getnextslot(TableScanDesc scan,
                                      ScanDirection direction,
                                      TupleTableSlot *slot);

extern Size optimized_parallelscan_estimate(Relation rel);

/* DML functions */
extern void optimized_tuple_insert(Relation relation,
                                  TupleTableSlot *slot,
                                  CommandId cid,
                                  int options,
                                  struct BulkInsertStateData *bistate);

extern TM_Result optimized_tuple_delete(Relation relation,
                                       ItemPointer tid,
                                       CommandId cid,
                                       Snapshot snapshot,
                                       Snapshot crosscheck,
                                       bool wait,
                                       TM_FailureData *tmfd,
                                       bool changingPart);

extern TM_Result optimized_tuple_update(Relation relation,
                                       ItemPointer otid,
                                       TupleTableSlot *slot,
                                       CommandId cid,
                                       Snapshot snapshot,
                                       Snapshot crosscheck,
                                       bool wait,
                                       TM_FailureData *tmfd,
                                       LockTupleMode *lockmode,
                                       TU_UpdateIndexes *update_indexes);

extern bool optimized_relation_needs_toast_table(Relation rel);

/* Index fetch functions */
extern struct IndexFetchTableData *optimized_index_fetch_begin(Relation rel);

extern void optimized_index_fetch_reset(struct IndexFetchTableData *scan);

extern void optimized_index_fetch_end(struct IndexFetchTableData *scan);

extern bool optimized_index_fetch_tuple(struct IndexFetchTableData *scan,
                                       ItemPointer tid,
                                       Snapshot snapshot,
                                       TupleTableSlot *slot,
                                       bool *call_again,
                                       bool *all_dead);

/* Slot callback functions */
extern const TupleTableSlotOps *optimized_slot_callbacks(Relation relation);

#endif /* ORF_FUNCTIONS_H */
