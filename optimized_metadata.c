/*
 * optimized_metadata.c
 *      Optimized storage metadata management
 */

#include "postgres.h"
#include "access/htup.h"
#include "access/table.h"
#include "access/tupdesc.h"
#include "catalog/pg_optimized_table_metadata.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "access/htup_details.h"
#include "utils/lsyscache.h"
#include "access/heapam.h"
#include "access/genam.h"
#include "catalog/index.h"
#include "access/relation.h"

/* Initialize metadata for a new table */
Datum
init_optimized_table_metadata(PG_FUNCTION_ARGS)
{
    Oid relid = PG_GETARG_OID(0);
    Relation rel;
    TupleDesc tupdesc;
    int fixed_col_count = 0;
    int var_col_count = 0;
    int i;
    int16 *fixed_col_offsets;
    int16 *offset_ptr;
    Datum values[4];
    bool nulls[4] = {false, false, false, false};
    Relation metadata_rel;
    HeapTuple tuple;

    /* Open the relation */
    rel = relation_open(relid, AccessShareLock);
    tupdesc = RelationGetDescr(rel);

    /* Count fixed and variable length columns */
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute att = TupleDescAttr(tupdesc, i);
        if (att->attlen > 0)
            fixed_col_count++;
        else
            var_col_count++;
    }

    /* Calculate fixed column offsets */
    fixed_col_offsets = palloc0(fixed_col_count * sizeof(int16));
    offset_ptr = fixed_col_offsets;
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute att = TupleDescAttr(tupdesc, i);
        if (att->attlen > 0)
        {
            *offset_ptr++ = att->attlen;
        }
    }

    /* Create the metadata tuple */
    values[0] = ObjectIdGetDatum(relid);
    values[1] = Int16GetDatum(fixed_col_count);
    values[2] = Int16GetDatum(var_col_count);

    /* Convert int16 array to Datum array for construct_array */
    Datum *offset_datums = palloc0(fixed_col_count * sizeof(Datum));
    for (i = 0; i < fixed_col_count; i++)
    {
        offset_datums[i] = Int16GetDatum(fixed_col_offsets[i]);
    }
    values[3] = PointerGetDatum(construct_array(offset_datums, fixed_col_count,
                                              INT2OID, sizeof(int16), true, 's'));
    pfree(offset_datums);

    /* Insert into metadata table */
    metadata_rel = table_open(OptimizedTableMetadataRelationId, RowExclusiveLock);
    tuple = heap_form_tuple(RelationGetDescr(metadata_rel), values, nulls);
    simple_heap_insert(metadata_rel, tuple);
    table_close(metadata_rel, RowExclusiveLock);

    /* Cleanup */
    pfree(fixed_col_offsets);
    relation_close(rel, AccessShareLock);

    PG_RETURN_VOID();
}

/* Update metadata when table structure changes */
Datum
update_optimized_table_metadata(PG_FUNCTION_ARGS)
{
    Oid relid = PG_GETARG_OID(0);
    /* Similar to init_optimized_table_metadata but updates existing row */
    /* TODO: Implement update logic */
    PG_RETURN_VOID();
}

/* Get metadata for a table */
Datum
get_optimized_table_metadata(PG_FUNCTION_ARGS)
{
    Oid relid = PG_GETARG_OID(0);
    Relation metadata_rel;
    HeapTuple tuple;
    TupleDesc tupdesc;
    Datum values[3];
    bool nulls[3] = {false, false, false};

    /* Open metadata table */
    metadata_rel = table_open(OptimizedTableMetadataRelationId, AccessShareLock);
    tupdesc = RelationGetDescr(metadata_rel);

    /* Look up the row */
    tuple = get_catalog_object_by_oid(metadata_rel, Anum_pg_optimized_table_metadata_relid, relid);
    if (!tuple)
        PG_RETURN_NULL();

    /* Extract values */
    values[0] = heap_getattr(tuple, Anum_pg_optimized_table_metadata_fixed_col_count,
                           tupdesc, &nulls[0]);
    values[1] = heap_getattr(tuple, Anum_pg_optimized_table_metadata_var_col_count,
                           tupdesc, &nulls[1]);
    values[2] = heap_getattr(tuple, Anum_pg_optimized_table_metadata_fixed_col_offsets,
                           tupdesc, &nulls[2]);

    /* Return the tuple as a composite type */
    PointerGetDatum(tuple->t_data);
}
