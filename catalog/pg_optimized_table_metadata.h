/*-------------------------------------------------------------------------
 *
 * pg_optimized_table_metadata.h
 *      definition of the "optimized table metadata" system catalog
 *      (pg_optimized_table_metadata)
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_optimized_table_metadata.h
 *
 * NOTES
 *      The Catalog.pm module reads this file and derives schema
 *      information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_OPTIMIZED_TABLE_METADATA_H
#define PG_OPTIMIZED_TABLE_METADATA_H

#include "catalog/genbki.h"
#include "catalog/pg_optimized_table_metadata_d.h"

/* ----------------
 *        pg_optimized_table_metadata definition.  cpp turns this into
 *        typedef struct FormData_pg_optimized_table_metadata
 * ----------------
 */
CATALOG(pg_optimized_table_metadata,12345,OptimizedTableMetadataRelationId)
{
    Oid         relid;              /* relation OID */
    int16       fixed_col_count;    /* number of fixed-length columns */
    int16       var_col_count;      /* number of variable-length columns */
    int2vector  fixed_col_offsets;  /* offsets to fixed-length columns */
} FormData_pg_optimized_table_metadata;

/* ----------------
 *        Form_pg_optimized_table_metadata corresponds to a pointer to a tuple with
 *        the format of pg_optimized_table_metadata relation.
 * ----------------
 */
typedef FormData_pg_optimized_table_metadata *Form_pg_optimized_table_metadata;

DECLARE_UNIQUE_INDEX(pg_optimized_table_metadata_relid_index, 12346, OptimizedTableMetadataRelidIndexId, pg_optimized_table_metadata, on pg_optimized_table_metadata using btree (relid oid_ops));

#endif                            /* PG_OPTIMIZED_TABLE_METADATA_H */