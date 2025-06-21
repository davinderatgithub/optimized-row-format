#include "postgres.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/pg_type.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "access/htup.h"
#include "access/tupdesc.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "optimized_row_format.h"

/*
 * Note on Column Order Mapping:
 * We deliberately avoid storing an explicit mapping between user-defined column order
 * and physical storage order for several reasons:
 * 1. The mapping is deterministic - fixed-length columns first, then variable-length
 * 2. Original column order is already in pg_attribute
 * 3. Additional mapping would increase metadata overhead and complexity
 * 4. Physical position can be calculated on-the-fly using column attributes
 *
 * Calculate the physical position of a column in the optimized storage format.
 * This function determines where a column is stored based on its type and original position.
 *
 * The physical layout is:
 * 1. Fixed-length columns first, in their original order
 * 2. Variable-length columns last, in their original order
 */
int
get_physical_position(Oid relid, int attnum)
{
    Relation rel;
    TupleDesc tupdesc;
    int physical_pos = 0;
    int i;
    Form_pg_attribute attr;

    /* Open the relation */
    rel = relation_open(relid, AccessShareLock);
    tupdesc = RelationGetDescr(rel);

    /* Get the attribute */
    attr = TupleDescAttr(tupdesc, attnum - 1);

    /* For fixed-length columns, position is based on original order */
    if (!attr->attisdropped && attr->attlen != -1)
    {
        for (i = 0; i < attnum - 1; i++)
        {
            Form_pg_attribute curr_attr = TupleDescAttr(tupdesc, i);
            if (!curr_attr->attisdropped && curr_attr->attlen != -1)
                physical_pos++;
        }
    }
    /* For variable-length columns, position is after all fixed-length columns */
    else
    {
        for (i = 0; i < tupdesc->natts; i++)
        {
            Form_pg_attribute curr_attr = TupleDescAttr(tupdesc, i);
            if (!curr_attr->attisdropped && curr_attr->attlen != -1)
                physical_pos++;
        }
        for (i = 0; i < attnum - 1; i++)
        {
            Form_pg_attribute curr_attr = TupleDescAttr(tupdesc, i);
            if (!curr_attr->attisdropped && curr_attr->attlen == -1)
                physical_pos++;
        }
    }

    relation_close(rel, AccessShareLock);
    return physical_pos;
}

/*
 * Calculate the offset to a column's data in the optimized storage format.
 * This function handles both fixed and variable-length columns.
 */
uint32
get_column_offset(TupleDesc tupleDesc, HeapTuple tuple, int attnum)
{
    OptimizedTupleHeader tup = (OptimizedTupleHeader) tuple->t_data;
    int physical_pos = get_physical_position(tuple->t_tableOid, attnum);
    Form_pg_attribute attr = TupleDescAttr(tupleDesc, attnum - 1);
    Relation rel;
    OptimizedStorageMetadata *metadata;

    /* Open the relation to get metadata */
    rel = relation_open(tuple->t_tableOid, AccessShareLock);
    metadata = get_optimized_storage_metadata(rel);

    /* For fixed-length columns, offset is based on position */
    if (!attr->attisdropped && attr->attlen != -1)
    {
        relation_close(rel, AccessShareLock);
        return physical_pos * attr->attlen;
    }
    /* For variable-length columns, use the offset array */
    else
    {
        uint32 *var_offsets = OPTIMIZED_TUPLE_VAR_OFFSETS(tup, metadata);
        relation_close(rel, AccessShareLock);
        return var_offsets[physical_pos];
    }
}

/*
 * Get optimized storage metadata for a relation.
 * This function creates and returns metadata about the relation's column layout.
 */
OptimizedStorageMetadata *
get_optimized_storage_metadata(Relation rel)
{
    TupleDesc tupdesc = RelationGetDescr(rel);
    OptimizedStorageMetadata *metadata;
    int i;
    int fixed_pos = 0;
    int var_pos = 0;

    /* Allocate metadata structure */
    metadata = palloc0(sizeof(OptimizedStorageMetadata));
    metadata->relid = RelationGetRelid(rel);
    metadata->natts = tupdesc->natts;
    metadata->phys_positions = palloc0(tupdesc->natts * sizeof(int));
    metadata->fixed_offsets = palloc0(tupdesc->natts * sizeof(Size));

    /* First pass: count variable columns and calculate fixed data length */
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
        if (!attr->attisdropped)
        {
            if (attr->attlen > 0)
            {
                metadata->phys_positions[i] = fixed_pos;
                metadata->fixed_offsets[i] = metadata->fixed_data_len;
                metadata->fixed_data_len += attr->attlen;
                fixed_pos++;
            }
            else
            {
                metadata->var_col_count++;
            }
        }
    }

    /* Second pass: set physical positions for variable columns */
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
        if (!attr->attisdropped && attr->attlen == -1)
        {
            metadata->phys_positions[i] = var_pos;
            var_pos++;
        }
    }

    return metadata;
}

/*
 * Free optimized storage metadata.
 */
void
free_optimized_storage_metadata(OptimizedStorageMetadata *metadata)
{
    if (metadata)
    {
        if (metadata->phys_positions)
            pfree(metadata->phys_positions);
        if (metadata->fixed_offsets)
            pfree(metadata->fixed_offsets);
        pfree(metadata);
    }
}