#ifndef ORF_UTILS_H
#define ORF_UTILS_H

#include "postgres.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "utils/relcache.h"
#include "optimized_row_format.h"

/* Function declarations from orf_utils.c */
OptimizedColumnMapCache *build_column_cache(TupleDesc tupleDesc);
Datum optimized_extract_attribute(HeapTuple tuple, int attnum, TupleDesc tupleDesc, 
                                  OptimizedColumnMapCache *cache, bool *isnull);
Datum optimized_extract_attribute_no_cache(HeapTuple tuple, int attnum, TupleDesc tupleDesc, bool *isnull);
Datum optimized_getattr(HeapTuple tuple, int attnum, TupleDesc tupleDesc, bool *isnull);
OffsetEncodingType choose_offset_encoding(Size estimated_tuple_size, int var_col_count);
bool optimized_relation_needs_toast_table(Relation rel);

#endif /* ORF_UTILS_H */
