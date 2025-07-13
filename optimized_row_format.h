#ifndef OPTIMIZED_ROW_FORMAT_H
#define OPTIMIZED_ROW_FORMAT_H

#include "postgres.h"
#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "access/htup.h"
#include "access/table.h"

/*
 * We will use the standard HeapTupleHeader for our optimized format to ensure
 * MVCC compatibility. The optimization is in the data layout that follows
 * the header.
 */
typedef HeapTupleHeaderData OptimizedTupleHeaderData;
typedef OptimizedTupleHeaderData *OptimizedTupleHeader;

/* Size of the optimized tuple header */
#define SizeofOptimizedTupleHeader offsetof(HeapTupleHeaderData, t_bits)

/*
 * Cache structure for column position mappings to eliminate O(N) lookups
 * This is stored in the relation's rd_amcache for efficient access
 */
typedef struct OptimizedColumnMapCache
{
    int natts;                    /* Number of attributes */
    uint32 *fixed_offsets;       /* Array of fixed-length column offsets */
    int *var_indexes;            /* Array of variable-length column indexes */
    Size fixed_data_len;         /* Total length of fixed-length data */
    int var_col_count;           /* Number of variable-length columns */
} OptimizedColumnMapCache;

#endif /* OPTIMIZED_ROW_FORMAT_H */