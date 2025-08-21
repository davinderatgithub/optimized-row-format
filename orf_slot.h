#ifndef ORF_SLOT_H
#define ORF_SLOT_H

#include "postgres.h"
#include "executor/tuptable.h"
#include "optimized_row_format.h"

/* Forward declarations for slot functions */
const TupleTableSlotOps *optimized_slot_callbacks(Relation relation);
void optimized_getsomeattrs(TupleTableSlot *slot, int natts);
Datum optimized_getattr_for_slot(TupleTableSlot *slot, int attnum, bool *isnull);
void optimized_slot_init_cache(OptimizedTupleTableSlot *opt_slot);
Datum optimized_getattr_direct(OptimizedTupleTableSlot *opt_slot, int attnum, bool *isnull);


#endif /* ORF_SLOT_H */
