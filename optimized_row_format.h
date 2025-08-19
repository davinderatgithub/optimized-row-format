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

/* Flags in t_infomask2 for optimized row format features */
#define OPTIMIZED_OFFSET_16BIT 0x8000  /* Use 16-bit offset encoding */

/*
 * Storage optimization flags for offset array encoding
 */
typedef enum
{
    OFFSET_ENCODING_32BIT = 0,    /* Standard 4-byte offsets */
    OFFSET_ENCODING_16BIT = 1     /* Compressed 2-byte offsets */
} OffsetEncodingType;

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
    OffsetEncodingType encoding;  /* Offset array encoding type */
} OptimizedColumnMapCache;

#endif /* OPTIMIZED_ROW_FORMAT_H */