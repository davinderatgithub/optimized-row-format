#ifndef OPTIMIZED_ROW_FORMAT_H
#define OPTIMIZED_ROW_FORMAT_H

#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/tupdesc.h"
#include "executor/tuptable.h"

typedef enum
{
    OFFSET_ENCODING_32BIT = 0,    /* Standard 4-byte offsets */
    OFFSET_ENCODING_16BIT = 1     /* Compressed 2-byte offsets */
} OffsetEncodingType;

#define OPTIMIZED_OFFSET_16BIT 0x8000  /* Use 16-bit offset encoding */

typedef HeapTupleHeaderData OptimizedTupleHeaderData;
typedef OptimizedTupleHeaderData *OptimizedTupleHeader;

typedef struct OptimizedColumnMapCache
{
    int natts;
    uint32 *fixed_offsets;
    int *var_indexes;
    Size fixed_data_len;
    int var_col_count;
    OffsetEncodingType encoding;
} OptimizedColumnMapCache;

/* --- Custom scan descriptor that embeds TableScanDescData --- */
typedef struct OptimizedScanDescData
{
    TableScanDescData rs_base;  /* AM independent part of the descriptor */
	HeapTupleData o_ctup;       /* Current tuple in scan */

    /* Private scan state for optimized row format */
    Relation rel;
    BlockNumber nblocks;
    BlockNumber currBlock;
    OffsetNumber currOffset;
    Buffer currBuffer;
    OptimizedColumnMapCache *column_cache;  /* Cache for O(1) attribute access */
} OptimizedScanDescData;

typedef OptimizedScanDescData *OptimizedScanDesc;

/* Custom tuple table slot for optimized row format with projection optimization */
typedef struct OptimizedTupleTableSlot
{
	TupleTableSlot base;		/* Base slot structure */
	HeapTuple opt_tuple;		/* The optimized tuple data */
	bool *attr_cached;			/* Track which attributes have been extracted */
	OptimizedColumnMapCache *column_cache;  /* Cache for O(1) attribute access */
	
	/* PERFORMANCE OPTIMIZATION: Cached data to reduce memory indirection */
	OptimizedTupleHeader cached_header;  /* Cached tuple header */
	char *fixed_data_start;      		/* Cached pointer to fixed data section */
	uint32 *var_offsets;         		/* Cached pointer to variable offsets array */
	uint32 var_col_count;        		/* Cached variable column count */
	bool cache_valid;            		/* Whether cached pointers are valid */
} OptimizedTupleTableSlot;

#define SizeofOptimizedTupleHeader offsetof(HeapTupleHeaderData, t_bits)

#endif   /* OPTIMIZED_ROW_FORMAT_H */