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

#endif /* OPTIMIZED_ROW_FORMAT_H */