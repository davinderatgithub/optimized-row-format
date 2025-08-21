#ifndef ORF_DML_H
#define ORF_DML_H

#include "postgres.h"
#include "access/heapam.h"
#include "access/xact.h"
#include "executor/tuptable.h"
#include "miscadmin.h"

/* Function declarations for DML operations */
void optimized_tuple_insert(Relation relation, TupleTableSlot *slot,
                            CommandId cid, int options, struct BulkInsertStateData *bistate);

TM_Result optimized_tuple_delete(Relation relation, ItemPointer tid,
                                 CommandId cid, Snapshot crosscheck, Snapshot snapshot,
                                 bool wait, TM_FailureData *tmfd, bool changingPart);

TM_Result optimized_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
                                 CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait,
                                 TM_FailureData *tmfd, LockTupleMode *lockmode,
                                 TU_UpdateIndexes *update_indexes);

#endif /* ORF_DML_H */
