#ifndef OPTIMIZED_STORAGE_H
#define OPTIMIZED_STORAGE_H

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
    /* Variable-length column offsets array follows the null bitmap */
    uint32      var_col_offsets[FLEXIBLE_ARRAY_MEMBER]; /* offsets to variable-length columns */
} OptimizedTupleHeaderData;

typedef OptimizedTupleHeaderData *OptimizedTupleHeader;

/* Size of the optimized tuple header */
#define SizeofOptimizedTupleHeader offsetof(OptimizedTupleHeaderData, t_bits)

/* Macros for accessing different parts of the tuple */
#define OPTIMIZED_TUPLE_NULL_BITMAP(tup) \
    ((bits8 *) ((char *)(tup) + (tup)->t_hoff))

#define OPTIMIZED_TUPLE_VAR_OFFSETS(tup) \
    ((uint32 *) ((char *)OPTIMIZED_TUPLE_NULL_BITMAP(tup) + \
                 BITMAPLEN(HeapTupleHeaderGetNatts(tup))))

#define OPTIMIZED_TUPLE_FIXED_DATA(tup) \
    ((char *) ((char *)OPTIMIZED_TUPLE_VAR_OFFSETS(tup) + \
               MAXALIGN(sizeof(uint32) * \
                       get_var_col_count(tup->t_tableOid))))

#define OPTIMIZED_TUPLE_VAR_DATA(tup) \
    ((char *) ((char *)OPTIMIZED_TUPLE_FIXED_DATA(tup) + \
               MAXALIGN(get_fixed_data_len(tup->t_tableOid))))

/* Helper macros for getting offsets */
#define OPTIMIZED_FIXED_COL_OFFSET(tup, attnum) \
    (get_physical_position((tup)->t_tableOid, attnum) * \
     TupleDescAttr(RelationGetDescr(relation_open((tup)->t_tableOid, AccessShareLock)), \
                  attnum - 1)->attlen)

#define OPTIMIZED_VAR_COL_OFFSET(tup, attnum) \
    (OPTIMIZED_TUPLE_VAR_OFFSETS(tup)[get_physical_position((tup)->t_tableOid, attnum)])

/* Function declarations */
extern int get_physical_position(Oid relid, int attnum);
extern uint32 get_column_offset(HeapTuple tuple, int attnum);
extern int get_var_col_count(Oid relid);
extern int get_fixed_data_len(Oid relid);

#endif /* OPTIMIZED_STORAGE_H */