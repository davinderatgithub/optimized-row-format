#include "postgres.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_type.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "access/htup.h"
#include "access/tupdesc.h"
#include "utils/relcache.h"
#include "utils/syscache.h"

#include "optimized_layout.h"

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

    /* Open the relation */
    rel = relation_open(relid, AccessShareLock);
    tupdesc = RelationGetDescr(rel);

    /* Get the attribute */
    Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

    /* For fixed-length columns, position is based on original order */
    if (!attr->attisdropped && !VARLENA_ATT_IS_EXTERNAL(attr))
    {
        for (i = 0; i < attnum - 1; i++)
        {
            Form_pg_attribute curr_attr = TupleDescAttr(tupdesc, i);
            if (!curr_attr->attisdropped && !VARLENA_ATT_IS_EXTERNAL(curr_attr))
                physical_pos++;
        }
    }
    /* For variable-length columns, position is after all fixed-length columns */
    else
    {
        for (i = 0; i < tupdesc->natts; i++)
        {
            Form_pg_attribute curr_attr = TupleDescAttr(tupdesc, i);
            if (!curr_attr->attisdropped && !VARLENA_ATT_IS_EXTERNAL(curr_attr))
                physical_pos++;
        }
        for (i = 0; i < attnum - 1; i++)
        {
            Form_pg_attribute curr_attr = TupleDescAttr(tupdesc, i);
            if (!curr_attr->attisdropped && VARLENA_ATT_IS_EXTERNAL(curr_attr))
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
get_column_offset(HeapTuple tuple, int attnum)
{
    OptimizedTupleHeader tup = (OptimizedTupleHeader) tuple->t_data;
    int physical_pos = get_physical_position(tuple->t_tableOid, attnum);
    Form_pg_attribute attr = TupleDescAttr(tuple->t_tableOid, attnum - 1);

    /* For fixed-length columns, offset is based on position */
    if (!attr->attisdropped && !VARLENA_ATT_IS_EXTERNAL(attr))
    {
        return physical_pos * attr->attlen;
    }
    /* For variable-length columns, use the offset array */
    else
    {
        return tup->var_col_offsets[physical_pos];
    }
}