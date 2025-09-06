#include "postgres.h"
#include "access/htup_details.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "catalog/pg_type.h"
#include "utils/lsyscache.h"
#include "access/detoast.h"

#include "optimized_row_format.h"
#include "orf_debug.h"
#include "orf_functions.h"

/*
 * Custom logging for optimized row format extension
 * DISABLED for testing - uncomment to enable debugging
 */
// #define OPTIMIZED_LOG(fmt, ...) \
//     elog(NOTICE, "OPTIMIZED_DEBUG: " fmt, ##__VA_ARGS__)
#define OPTIMIZED_LOG(fmt, ...) ORF_DEBUG_INFO(utils, fmt, ##__VA_ARGS__)

/*
 * build_column_cache
 *      Build a cache of column position mappings to eliminate O(N) lookups.
 *      This pre-computes which columns are fixed vs variable-length and their
 *      positions in the optimized tuple format.
 */
OptimizedColumnMapCache *
build_column_cache(TupleDesc tupleDesc)
{
	OptimizedColumnMapCache *cache;
	int i, var_col_index = 0;
	Size current_fixed_offset = 0;
	MemoryContext oldcontext;
	
	OPTIMIZED_LOG("build_column_cache: Building cache for %d attributes", tupleDesc->natts);
	
	/* 
	 * CRITICAL FIX: Allocate cache in CacheMemoryContext to prevent it from being
	 * freed when the current memory context is reset. The cache needs to persist
	 * for the lifetime of the relation as it's stored in rel->rd_amcache.
	 */
	oldcontext = MemoryContextSwitchTo(CacheMemoryContext);
	
	cache = (OptimizedColumnMapCache *) palloc0(sizeof(OptimizedColumnMapCache));
	cache->natts = tupleDesc->natts;
	
	/* Allocate arrays for mappings */
	cache->fixed_offsets = (uint32 *) palloc0(tupleDesc->natts * sizeof(uint32));
	cache->var_indexes = (int *) palloc0(tupleDesc->natts * sizeof(int));
	
	MemoryContextSwitchTo(oldcontext);
	
	/* Analyze each column and build the cache */
	for (i = 0; i < tupleDesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupleDesc, i);
		
		if (att->attisdropped)
		{
			/* Dropped columns: mark as invalid */
			cache->fixed_offsets[i] = UINT32_MAX;
			cache->var_indexes[i] = -1;
			continue;
		}
			
		if (att->attlen > 0) /* Fixed-length column */
		{
			cache->fixed_offsets[i] = current_fixed_offset;
			cache->var_indexes[i] = -1; /* Not a variable column */
			current_fixed_offset += att->attlen;
			
			OPTIMIZED_LOG("build_column_cache: Column %d (%s) fixed-length, offset=%u", 
						  i + 1, NameStr(att->attname), cache->fixed_offsets[i]);
		}
		else /* Variable-length column */
		{
			cache->fixed_offsets[i] = UINT32_MAX; /* Not a fixed column */
			cache->var_indexes[i] = var_col_index++;
			
			OPTIMIZED_LOG("build_column_cache: Column %d (%s) variable-length, var_index=%d", 
						  i + 1, NameStr(att->attname), cache->var_indexes[i]);
		}
	}
	
	cache->fixed_data_len = current_fixed_offset;
	cache->var_col_count = var_col_index;
	
	OPTIMIZED_LOG("build_column_cache: Cache built - fixed_data_len=%zu, var_col_count=%d", 
				  cache->fixed_data_len, cache->var_col_count);
	
	return cache;
}

/* Forward declaration */
Datum optimized_extract_attribute(HeapTuple tuple, int attnum, TupleDesc tupleDesc, 
                                 OptimizedColumnMapCache *cache, bool *isnull);

/*
 * optimized_extract_attribute_no_cache
 *      Fallback function for cases where we don't have a cache available.
 *      This uses the old O(N) method but only for materialization operations.
 */
Datum
optimized_extract_attribute_no_cache(HeapTuple tuple, int attnum, TupleDesc tupleDesc, bool *isnull)
{

	/* Build a temporary cache for this operation */
	OptimizedColumnMapCache *temp_cache = build_column_cache(tupleDesc);
	Datum result;
	
	result = optimized_extract_attribute(tuple, attnum, tupleDesc, temp_cache, isnull);
	
	/* Clean up temporary cache */
	pfree(temp_cache->fixed_offsets);
	pfree(temp_cache->var_indexes);
	pfree(temp_cache);
	
	return result;
}


/*
 * optimized_extract_attribute
 *      Extract a single attribute from an optimized tuple format.
 *      Uses pre-computed cache for O(1) attribute access performance.
 */
Datum
optimized_extract_attribute(HeapTuple tuple, int attnum, TupleDesc tupleDesc, 
                           OptimizedColumnMapCache *cache, bool *isnull)
{
    OptimizedTupleHeader header = (OptimizedTupleHeader) tuple->t_data;
    Form_pg_attribute att = TupleDescAttr(tupleDesc, attnum - 1);
    
    /* Fallback variable declarations - moved to top for C99 compatibility */
    char *null_bitmap = NULL;
    uint32 *var_col_count_ptr;
    uint32 var_col_count;
    uint32 *var_offsets;
    char *fixed_data;
    char *data_ptr;
    uint32 fixed_off = 0;
    int target_var_index = -1;
    int i;
    int var_col_index;
    
    /*
     * PERFORMANCE OPTIMIZATION: Fast validation with minimal overhead
     */
    if (cache && cache->fixed_offsets && cache->var_indexes && 
        cache->natts == tupleDesc->natts && attnum >= 1 && attnum <= cache->natts)
    {
        /*
         * FAST PATH: Use cached offsets for O(1) access
         */
        if (att->attlen > 0) /* Fixed-length column */
        {
            /* Fast path for fixed-length columns - most common case */
            uint32 fixed_off = cache->fixed_offsets[attnum - 1];
            
            /* Calculate fixed data pointer with minimal overhead */
            char *data_start = (char *) header + header->t_hoff;
            char *fixed_data;
            
            /* Account for NULL bitmap if present */
            if (header->t_infomask & HEAP_HASNULL)
            {
                char *null_bitmap = data_start;
                uint32 *var_col_count_ptr = (uint32 *) (null_bitmap + MAXALIGN(BITMAPLEN(tupleDesc->natts)));
                uint32 var_col_count = *var_col_count_ptr;
                uint32 *var_offsets = (uint32 *) ((char *) var_col_count_ptr + MAXALIGN(sizeof(uint32)));
                
                /* Use correct offset array size based on encoding */
                OffsetEncodingType encoding = (header->t_infomask2 & OPTIMIZED_OFFSET_16BIT) ? 
                    OFFSET_ENCODING_16BIT : OFFSET_ENCODING_32BIT;
                size_t offset_array_size = (encoding == OFFSET_ENCODING_16BIT) ? 
                    MAXALIGN(var_col_count * sizeof(uint16)) : 
                    MAXALIGN(var_col_count * sizeof(uint32));
                fixed_data = (char *)var_offsets + offset_array_size;
            }
            else
            {
                uint32 *var_col_count_ptr = (uint32 *) MAXALIGN(data_start);
                uint32 var_col_count = *var_col_count_ptr;
                uint32 *var_offsets = (uint32 *) ((char *) var_col_count_ptr + MAXALIGN(sizeof(uint32)));
                
                /* Use correct offset array size based on encoding */
                OffsetEncodingType encoding = (header->t_infomask2 & OPTIMIZED_OFFSET_16BIT) ? 
                    OFFSET_ENCODING_16BIT : OFFSET_ENCODING_32BIT;
                size_t offset_array_size = (encoding == OFFSET_ENCODING_16BIT) ? 
                    MAXALIGN(var_col_count * sizeof(uint16)) : 
                    MAXALIGN(var_col_count * sizeof(uint32));
                fixed_data = (char *)var_offsets + offset_array_size;
            }
            
            char *data_ptr = fixed_data + fixed_off;
            *isnull = false;

            /* Optimized extraction for common integer types */
            if (att->attbyval)
            {
                switch (att->attlen)
                {
                    case sizeof(int32):
                        return Int32GetDatum(*((int32 *) data_ptr));
                    case sizeof(int16):
                        return Int16GetDatum(*((int16 *) data_ptr));
                    case sizeof(char):
                        return CharGetDatum(*((char *) data_ptr));
#if SIZEOF_DATUM == 8
                    case sizeof(Datum):
                        return *((Datum *) data_ptr);
#endif
                    default:
                        break; /* Fall through to slower path */
                }
            }
            /* For non-byval fixed types, use original logic */
            return PointerGetDatum(data_ptr);
        }
        else /* Variable-length column */
        {
            /* Variable columns: use cached var_index */
            int target_var_index = cache->var_indexes[attnum - 1];
            
            char *data_start = (char *) header + header->t_hoff;
            uint32 *var_col_count_ptr;
            uint32 *var_offsets;
            
            /* Account for NULL bitmap if present */
            if (header->t_infomask & HEAP_HASNULL)
            {
                char *null_bitmap = data_start;
                var_col_count_ptr = (uint32 *) (null_bitmap + MAXALIGN(BITMAPLEN(tupleDesc->natts)));
                var_offsets = (uint32 *) ((char *) var_col_count_ptr + MAXALIGN(sizeof(uint32)));
            }
            else
            {
                var_col_count_ptr = (uint32 *) MAXALIGN(data_start);
                var_offsets = (uint32 *) ((char *) var_col_count_ptr + MAXALIGN(sizeof(uint32)));
            }
            
            uint32 var_col_count = *var_col_count_ptr;
            
            /* Check if this specific attribute is NULL using the null bitmap */
            if (header->t_infomask & HEAP_HASNULL)
            {
                char *null_bitmap = data_start;
                int byte_offset = (attnum - 1) / 8;
                int bit_offset = (attnum - 1) % 8;
                
                /* Check if the bit is 0 (NULL) or 1 (not NULL) */
                if (!(null_bitmap[byte_offset] & (1 << bit_offset)))
                {
                    *isnull = true;
                    return (Datum) 0;  /* Return NULL datum */
                }
            }
            
            *isnull = false;
            if (target_var_index < var_col_count)
            {
                /* Read offset based on encoding type */
                OffsetEncodingType encoding = (header->t_infomask2 & OPTIMIZED_OFFSET_16BIT) ? 
                    OFFSET_ENCODING_16BIT : OFFSET_ENCODING_32BIT;
                
                uint32 absolute_offset;
                if (encoding == OFFSET_ENCODING_16BIT) {
                    uint16 *var_offsets_16 = (uint16 *) var_offsets;
                    absolute_offset = var_offsets_16[target_var_index];
                } else {
                    absolute_offset = var_offsets[target_var_index];
                }
                
                char *var_data_ptr = (char *)header + absolute_offset;
                return PointerGetDatum(var_data_ptr);
            }
            else
            {
                *isnull = true;
                return (Datum) NULL;
            }
        }
    }

    /*
     * FALLBACK: Original complex logic for compatibility
     * Only used when cache is invalid or for edge cases
     */
    var_col_index = 0;
    
    if (cache == NULL || cache->fixed_offsets == NULL || cache->var_indexes == NULL || 
        cache->natts != tupleDesc->natts || attnum < 1 || attnum > cache->natts)
    {
        /*
         * FALLBACK: O(N) computation when cache is unavailable
         * This is slower but ensures correctness
         */
        OPTIMIZED_LOG("optimized_extract_attribute: Cache invalid, using O(N) fallback");
        
        for (i = 0; i < tupleDesc->natts; i++)
        {
            Form_pg_attribute current_att = TupleDescAttr(tupleDesc, i);
            
            if (current_att->attisdropped)
                continue;
                
            if (current_att->attlen > 0) /* Fixed-length column */
            {
                if (i < attnum - 1)
                    fixed_off += current_att->attlen;
            }
            else /* Variable-length column */
            {
                if (i < attnum - 1)
                    var_col_index++;
                else if (i == attnum - 1)
                    target_var_index = var_col_index;
            }
        }
        
        OPTIMIZED_LOG("optimized_extract_attribute: O(N) computed fixed_off=%u, var_index=%d", fixed_off, target_var_index);
    }
    else
    {
        /*
         * O(1) CACHE LOOKUP: Use pre-computed column positions - FAST PATH!
         */
        if (att->attlen > 0) /* Fixed-length column */
        {
            fixed_off = cache->fixed_offsets[attnum - 1];
            OPTIMIZED_LOG("optimized_extract_attribute: Fixed column, cached offset=%u", fixed_off);
        }
        else /* Variable-length column */
        {
            target_var_index = cache->var_indexes[attnum - 1];
            OPTIMIZED_LOG("optimized_extract_attribute: Variable column, cached var_index=%d", target_var_index);
        }
    }

    /* Set up pointers to data sections */
    if (HeapTupleHasNulls(tuple))
    {
        /* Null bitmap is present */
        null_bitmap = (char *) header + header->t_hoff;
        var_col_count_ptr = (uint32 *) (null_bitmap + MAXALIGN(BITMAPLEN(tupleDesc->natts)));
    }
    else
    {
        /* No null bitmap - variable column count starts after header with proper alignment */
        char *data_start = (char *) header + header->t_hoff;
        var_col_count_ptr = (uint32 *) MAXALIGN(data_start);
    }

    var_col_count = *var_col_count_ptr;
    var_offsets = (uint32 *) ((char *) var_col_count_ptr + MAXALIGN(sizeof(uint32)));

    OPTIMIZED_LOG("optimized_extract_attribute: var_col_count=%u, natts=%d, hasnulls=%d",
         var_col_count, tupleDesc->natts, HeapTupleHasNulls(tuple));

    /* Fixed data starts immediately after the variable offsets array */
    /* CRITICAL FIX: Account for offset encoding type when calculating fixed data pointer */
    OffsetEncodingType encoding = (header->t_infomask2 & OPTIMIZED_OFFSET_16BIT) ? 
        OFFSET_ENCODING_16BIT : OFFSET_ENCODING_32BIT;
    
    size_t offset_size = (encoding == OFFSET_ENCODING_16BIT) ? sizeof(uint16) : sizeof(uint32);
    fixed_data = (char *)var_offsets + MAXALIGN(var_col_count * offset_size);

    OPTIMIZED_LOG("optimized_extract_attribute: fixed_data=%p", fixed_data);

    /* Check if this specific attribute is NULL using the null bitmap */
    if (null_bitmap != NULL)
    {
        int byte_offset = (attnum - 1) / 8;
        int bit_offset = (attnum - 1) % 8;
        
        /* Check if the bit is 0 (NULL) or 1 (not NULL) */
        if (!(null_bitmap[byte_offset] & (1 << bit_offset)))
        {
            *isnull = true;
            return (Datum) 0;  /* Return NULL datum */
        }
    }
    
    *isnull = false;

    /* Handle fixed-length columns */
    if (att->attlen > 0)
    {
        char val;
        int16 val16;
        int32 val32;
#if SIZEOF_DATUM == 8
        Datum val_datum;
#endif
        
        data_ptr = fixed_data + fixed_off;
        OPTIMIZED_LOG("optimized_extract_attribute: fixed column, offset=%u", fixed_off);

        if (att->attbyval)
        {
            /* Pass-by-value: extract the value based on length */
            switch (att->attlen)
            {
                case sizeof(char):
                    val = *((char *) data_ptr);
                    OPTIMIZED_LOG("optimized_extract_attribute: char value=%d", val);
                    return CharGetDatum(val);
                case sizeof(int16):
                    val16 = *((int16 *) data_ptr);
                    OPTIMIZED_LOG("optimized_extract_attribute: int16 value=%d", val16);
                    return Int16GetDatum(val16);
                case sizeof(int32):
                    val32 = *((int32 *) data_ptr);
                    OPTIMIZED_LOG("optimized_extract_attribute: int32 value=%d", val32);
                    return Int32GetDatum(val32);
#if SIZEOF_DATUM == 8
                case sizeof(Datum):
                    val_datum = *((Datum *) data_ptr);
                    OPTIMIZED_LOG("optimized_extract_attribute: Datum value=%ld", val_datum);
                    return val_datum;
#endif
                default:
                    elog(ERROR, "unsupported byval length: %d", att->attlen);
                    return (Datum) 0;
            }
        }
        else
        {
            /* Pass-by-reference: return pointer to the data */
            OPTIMIZED_LOG("optimized_extract_attribute: fixed pass-by-ref, data_ptr=%p", data_ptr);
            return PointerGetDatum(data_ptr);
        }
    }
    /* Handle variable-length columns */
    else
    {
        OPTIMIZED_LOG("optimized_extract_attribute: variable column, var_index=%d", target_var_index);

        /* Get the data from the variable section using the absolute offset */
        if (target_var_index < var_col_count)
        {
            /* Read offset encoding from tuple header */
            OffsetEncodingType encoding = (header->t_infomask2 & OPTIMIZED_OFFSET_16BIT) ? 
                OFFSET_ENCODING_16BIT : OFFSET_ENCODING_32BIT;
            
            uint32 absolute_offset;
            if (encoding == OFFSET_ENCODING_16BIT) {
                uint16 *var_offsets_16 = (uint16 *) var_offsets;
                absolute_offset = var_offsets_16[target_var_index];
                OPTIMIZED_LOG("16-bit offset read: var_offsets=%p, var_offsets_16=%p, target_var_index=%d, raw_value=%u", 
                             var_offsets, var_offsets_16, target_var_index, var_offsets_16[target_var_index]);
            } else {
                absolute_offset = var_offsets[target_var_index];
                OPTIMIZED_LOG("32-bit offset read: var_offsets=%p, target_var_index=%d, raw_value=%u", 
                             var_offsets, target_var_index, var_offsets[target_var_index]);
            }
            char *var_data_ptr = (char *)header + absolute_offset;
            OPTIMIZED_LOG("optimized_extract_attribute: encoding=%s, target_var_index=%d, absolute_offset=%u, var_data_ptr=%p", 
                         (encoding == OFFSET_ENCODING_16BIT) ? "16-bit" : "32-bit",
                         target_var_index, absolute_offset, var_data_ptr);
            
            /* 
             * CRITICAL FIX: The data is already in proper varlena format from when we stored it.
             * We stored the complete varlena structure (including length header) in our optimized format,
             * so we can return it directly as a pointer.
             * 
             * However, we need to ensure no TOAST compression flags are set incorrectly.
             */
            struct varlena *varlena_ptr = (struct varlena *) var_data_ptr;
            
            /* Validate varlena structure before returning */
            Size varsize = VARSIZE_ANY(varlena_ptr);
            OPTIMIZED_LOG("optimized_extract_attribute: varlena_ptr=%p, raw_size=%zu, first_4_bytes=0x%08x", 
                         varlena_ptr, varsize, *((uint32*)varlena_ptr));
            
            /* Check for obviously corrupted size */
            if (varsize > 1073741824 || varsize < 1) /* 1GB limit, minimum 1 byte */
            {
                elog(ERROR, "CORRUPTION: Invalid varlena size %zu at offset %u, first_4_bytes=0x%08x", 
                     varsize, absolute_offset, *((uint32*)varlena_ptr));
            }
            
            /* Debug check for TOAST compression flags */
            if (VARATT_IS_COMPRESSED(varlena_ptr) || VARATT_IS_EXTERNAL(varlena_ptr))
            {
                ORF_DEBUG_VERBOSE(utils, "PostgreSQL thinks varlena data is compressed! Size=%zu", varsize);
            }
            
            OPTIMIZED_LOG("optimized_extract_attribute: returning valid varlena data at %p, size=%zu", varlena_ptr, varsize);
            return PointerGetDatum(varlena_ptr);
        }
        else
        {
            OPTIMIZED_LOG("optimized_extract_attribute: var_index >= var_col_count, returning NULL");
            *isnull = true;
            return (Datum) NULL;
        }
    }
}

/*
 * optimized_getattr
 *      Extract an attribute of an optimized tuple and return it as a Datum.
 *      This works for either system or user attributes. The given attnum
 *      is properly range-checked.
 *
 *      If the field in question has a NULL value, we return a zero Datum
 *      and set *isnull == true. Otherwise, we set *isnull == false.
 *
 *      <tuple> is the pointer to the optimized tuple. <attnum> is the attribute
 *      number of the column (field) caller wants. <tupleDesc> is a pointer
 *      to the structure describing the row and all its fields.
 */
Datum
optimized_getattr(HeapTuple tuple, int attnum, TupleDesc tupleDesc, bool *isnull)
{
	Datum datum;
	Form_pg_attribute att;

	att = TupleDescAttr(tupleDesc, attnum - 1);
	datum = heap_getattr(tuple, attnum, tupleDesc, isnull);

	if (!*isnull && att->attlen == -1)
	{
		datum = PointerGetDatum(pg_detoast_datum((struct varlena *) DatumGetPointer(datum)));
	}

	return datum;
}


/*
 * choose_offset_encoding
 *      Determine the optimal offset encoding strategy based on tuple characteristics.
 *      Use 16-bit offsets for small tuples to reduce storage overhead.
 */
OffsetEncodingType
choose_offset_encoding(Size estimated_tuple_size, int var_col_count)
{
    /* Use 16-bit offsets if:
     * 1. Tuple size is small enough (< 32KB)
     * 2. We have variable columns to optimize
     * 3. Offset array would provide meaningful savings
     */
    if (estimated_tuple_size < 32768 && var_col_count > 0 && var_col_count >= 2)
    {
        OPTIMIZED_LOG("Using 16-bit offset encoding for tuple size %zu with %d var columns", 
                     estimated_tuple_size, var_col_count);
        return OFFSET_ENCODING_16BIT;
    }
    
    OPTIMIZED_LOG("Using 32-bit offset encoding for tuple size %zu with %d var columns", 
                 estimated_tuple_size, var_col_count);
    return OFFSET_ENCODING_32BIT;
}

bool
optimized_relation_needs_toast_table(Relation rel)
{
	/* For now, disable TOAST tables for this AM */
	return false;
}
