#ifndef OPTIMIZED_ROW_FORMAT_H
#define OPTIMIZED_ROW_FORMAT_H

#include "postgres.h"
#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "access/htup.h"
#include "access/table.h"

/* Optimized tuple header structure */
typedef struct OptimizedTupleHeaderData
{
    uint32      t_len;          /* total length of tuple */
    uint16      t_infomask;     /* various flag bits */
    uint16      t_infomask2;    /* number of attributes + flags */
    uint8       t_hoff;         /* offset to user data */
    bits8       t_bits[FLEXIBLE_ARRAY_MEMBER]; /* null bitmap */
} OptimizedTupleHeaderData;

typedef OptimizedTupleHeaderData *OptimizedTupleHeader;

/* Size of the optimized tuple header */
#define SizeofOptimizedTupleHeader offsetof(OptimizedTupleHeaderData, t_bits)

/* Metadata cache entry for a relation */
typedef struct OptimizedStorageMetadata
{
    Oid         relid;          /* relation OID */
    int         natts;          /* number of attributes */
    int         var_col_count;  /* number of variable-length columns */
    Size        fixed_data_len; /* total length of fixed-length data */
    int        *phys_positions; /* physical position mapping */
    Size       *fixed_offsets;  /* fixed column offsets */
} OptimizedStorageMetadata;

/* Safe macro to get null bitmap */
#define OPTIMIZED_TUPLE_NULL_BITMAP(tup) \
    ((bits8 *) ((char *)(tup) + MAXALIGN((tup)->t_hoff)))

/* Safe macro to get variable offsets array */
#define OPTIMIZED_TUPLE_VAR_OFFSETS(tup, metadata) \
    ((uint32 *) (MAXALIGN((char *)OPTIMIZED_TUPLE_NULL_BITMAP(tup) + \
                 BITMAPLEN((metadata)->natts))))

/* Safe macro to get fixed data section */
#define OPTIMIZED_TUPLE_FIXED_DATA(tup, metadata) \
    (MAXALIGN((char *)OPTIMIZED_TUPLE_VAR_OFFSETS(tup, metadata) + \
              ((metadata)->var_col_count * sizeof(uint32))))

/* Safe macro to get variable data section */
#define OPTIMIZED_TUPLE_VAR_DATA(tup, metadata) \
    (MAXALIGN((char *)OPTIMIZED_TUPLE_FIXED_DATA(tup, metadata) + \
              (metadata)->fixed_data_len))

/* Safe macro to get fixed column offset */
#define OPTIMIZED_FIXED_COL_OFFSET(metadata, attnum) \
    ((metadata)->fixed_offsets[attnum - 1])

/* Safe macro to get variable column offset */
#define OPTIMIZED_VAR_COL_OFFSET(tup, metadata, attnum) \
    (OPTIMIZED_TUPLE_VAR_OFFSETS(tup, metadata)[(metadata)->phys_positions[attnum - 1]])

/* Function declarations */
extern OptimizedStorageMetadata *get_optimized_storage_metadata(Relation rel);
extern void free_optimized_storage_metadata(OptimizedStorageMetadata *metadata);
extern int get_physical_position(Oid relid, int attnum);
extern uint32 get_column_offset(TupleDesc tupleDesc, HeapTuple tuple, int attnum);

#endif /* OPTIMIZED_ROW_FORMAT_H */