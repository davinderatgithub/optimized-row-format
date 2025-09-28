/*
 * orf_slot_ops.c
 *
 * Custom TupleTableSlot operations for optimized row format
 * Implements O(1) attribute access using segregated storage layout
 * Provides projection optimization through smart extraction
 */

#include "postgres.h"
#include "access/htup_details.h"
#include "executor/tuptable.h"
#include "utils/memutils.h"

#include "orf_slot_ops.h"
#include "orf_utils.h"
#include "orf_dml.h"
#include "orf_debug.h"

/*
 * Logging macros for slot operations
 */
#define ORF_SLOT_LOG(fmt, ...) ORF_DEBUG_INFO(slot, fmt, ##__VA_ARGS__)
#define ORF_SLOT_VERBOSE(fmt, ...) ORF_DEBUG_VERBOSE(slot, fmt, ##__VA_ARGS__)

/*
 * Basic slot operation functions
 */

void
tts_optimized_init(TupleTableSlot *slot)
{
    OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
    
    opt_slot->tuple = NULL;
    opt_slot->cache = NULL;
    opt_slot->tts_extracted = NULL;
    opt_slot->cache_valid = false;
    opt_slot->highest_requested = 0;
    
    ORF_SLOT_LOG("tts_optimized_init: Initialized optimized slot");
}

void
tts_optimized_release(TupleTableSlot *slot)
{
    OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
    
    /* Free the extraction tracking array */
    if (opt_slot->tts_extracted)
    {
        pfree(opt_slot->tts_extracted);
        opt_slot->tts_extracted = NULL;
    }
    
    /* Note: We don't free the cache - it's owned by the relation */
    /* Note: We don't free the tuple - it's managed by the slot system */
    
    ORF_SLOT_LOG("tts_optimized_release: Released optimized slot resources");
}

void
tts_optimized_clear(TupleTableSlot *slot)
{
    OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
    
    /* Free the heap tuple if it's owned by the slot */
    if (TTS_SHOULDFREE(slot) && opt_slot->tuple)
    {
        heap_freetuple(opt_slot->tuple);
        opt_slot->tuple = NULL;
    }
    
    /* Reset extraction tracking */
    if (opt_slot->tts_extracted)
    {
        memset(opt_slot->tts_extracted, false, 
               slot->tts_tupleDescriptor->natts * sizeof(bool));
    }
    
    /* Reset tracking state */
    opt_slot->highest_requested = 0;
    
    /* Clear base slot state */
    slot->tts_nvalid = 0;
    slot->tts_flags |= TTS_FLAG_EMPTY;
    
    ORF_SLOT_LOG("tts_optimized_clear: Cleared optimized slot");
}

/*
 * tts_optimized_getsomeattrs - Smart extraction leveraging O(1) random access
 * 
 * KEY INNOVATION: Unlike heap slots that extract sequentially (1→2→3→4→5),
 * we only extract the attributes that haven't been extracted yet.
 * This leverages our O(1) random access capability.
 */
void
tts_optimized_getsomeattrs(TupleTableSlot *slot, int natts)
{
    OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
    int attnum;
    int extracted_count = 0;
    
    /* 
     * CRITICAL FIX: Handle empty slots properly
     * During INSERT operations, the slot may be empty initially, but our copyslot 
     * function will materialize it first. For truly empty slots, we can't extract anything.
     */
    if (TTS_EMPTY(slot))
    {
        ORF_SLOT_LOG("tts_optimized_getsomeattrs: Slot is empty - cannot extract attributes");
        return;
    }
    
    Assert(natts > 0);
    Assert(natts <= slot->tts_tupleDescriptor->natts);
    
    /* If we already have all requested attributes, nothing to do */
    if (natts <= slot->tts_nvalid)
    {
        ORF_SLOT_VERBOSE("tts_optimized_getsomeattrs: Already have %d attributes (requested %d)", 
                        slot->tts_nvalid, natts);
        return;
    }
    
    /* Ensure we have extraction tracking array */
    if (!opt_slot->tts_extracted)
    {
        opt_slot->tts_extracted = (bool *) palloc0(
            slot->tts_tupleDescriptor->natts * sizeof(bool));
        ORF_SLOT_VERBOSE("tts_optimized_getsomeattrs: Allocated extraction tracking array");
    }
    
    ORF_SLOT_LOG("tts_optimized_getsomeattrs: Extracting up to %d attributes (current tts_nvalid=%d)", 
                natts, slot->tts_nvalid);
    
    /*
     * SMART EXTRACTION: Only extract attributes that haven't been extracted yet.
     * This is different from heap slots which extract sequentially 1→N.
     * We can extract any attribute directly using our cache.
     */
    for (attnum = 1; attnum <= natts; attnum++)
    {
        /* Skip if already extracted */
        if (opt_slot->tts_extracted[attnum - 1])
        {
            ORF_SLOT_VERBOSE("tts_optimized_getsomeattrs: Attribute %d already extracted", attnum);
            continue;
        }
            
        /* Extract this specific attribute using O(1) cache lookup */
        ORF_SLOT_VERBOSE("tts_optimized_getsomeattrs: Extracting attribute %d", attnum);
        
        slot->tts_values[attnum - 1] = optimized_extract_attribute(
            opt_slot->tuple,
            attnum,
            slot->tts_tupleDescriptor,
            opt_slot->cache,
            &slot->tts_isnull[attnum - 1]
        );
        
        /* Mark as extracted */
        opt_slot->tts_extracted[attnum - 1] = true;
        extracted_count++;
        
        ORF_SLOT_TRACK_ACCESS(slot, attnum);
    }
    
    /* Update tts_nvalid to reflect highest extracted attribute */
    if (natts > slot->tts_nvalid)
        slot->tts_nvalid = natts;
        
    /* Update highest requested tracking */
    if (natts > opt_slot->highest_requested)
        opt_slot->highest_requested = natts;
        
    ORF_SLOT_LOG("tts_optimized_getsomeattrs: Completed, extracted %d new attributes, tts_nvalid=%d", 
                extracted_count, slot->tts_nvalid);
}

Datum
tts_optimized_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
    OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
    
    Assert(!TTS_EMPTY(slot));
    
    /* System attributes are stored in the heap tuple header, same as heap */
    return heap_getsysattr(opt_slot->tuple, attnum, 
                          slot->tts_tupleDescriptor, isnull);
}

void
tts_optimized_materialize(TupleTableSlot *slot)
{
    OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
    MemoryContext oldContext;
    
    /* 
     * CRITICAL FIX: Handle empty slots during INSERT operations
     * During INSERT, PostgreSQL may call materialize on an empty slot that will be populated
     * with values. We need to handle this case gracefully.
     */
    if (TTS_EMPTY(slot))
    {
        ORF_SLOT_LOG("tts_optimized_materialize: Slot is empty - nothing to materialize");
        return;
    }
    
    /* If we already own the tuple, nothing to do */
    if (TTS_SHOULDFREE(slot))
    {
        ORF_SLOT_LOG("tts_optimized_materialize: Tuple already materialized");
        return;
    }
        
    oldContext = MemoryContextSwitchTo(slot->tts_mcxt);
    
    /*
     * DESIGN PRINCIPLE: Optimized slots should always contain optimized tuples
     * Materialization should never change the tuple format - just ensure ownership
     */
    
    if (opt_slot->tuple)
    {
        /*
         * OPTIMAL PATH: We have the original optimized tuple
         * Simply copy it to ensure ownership while preserving format
         * This maintains all projection optimizations and format benefits
         */
        opt_slot->tuple = heap_copytuple(opt_slot->tuple);
        ORF_SLOT_LOG("tts_optimized_materialize: Copied optimized tuple (format preserved)");
    }
    else
    {
        /*
         * VIRTUAL SLOT PATH: No physical tuple available
         * 
         * This is a legitimate scenario that occurs when:
         * 1. Slot contains computed/projected values (SELECT col1+col2)
         * 2. Join results combining multiple tables
         * 3. Virtual tuples created by executor nodes
         * 4. Trigger processing with modified tuples
         * 5. INSERT operations where values are set but no tuple exists yet
         * 
         * We create an optimized tuple from the slot's extracted values.
         * This preserves our optimized format even for virtual slots.
         */
        opt_slot->tuple = build_optimized_tuple_from_slot(NULL, slot);
        ORF_SLOT_LOG("tts_optimized_materialize: Created optimized tuple from virtual slot data");
    }
    
    MemoryContextSwitchTo(oldContext);
    
    /* Mark that we now own the tuple */
    slot->tts_flags |= TTS_FLAG_SHOULDFREE;
    
    ORF_SLOT_LOG("tts_optimized_materialize: Materialized optimized slot");
}

void
tts_optimized_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
    OptimizedTupleTableSlot *src_opt = (OptimizedTupleTableSlot *) srcslot;
    OptimizedTupleTableSlot *dst_opt = (OptimizedTupleTableSlot *) dstslot;
    
    /* Clear destination slot */
    ExecClearTuple(dstslot);
    
    /* 
     * CRITICAL FIX: If source slot has no tuple but has values (INSERT case),
     * materialize it first, just like heap implementation does.
     */
    if (!src_opt->tuple && !TTS_EMPTY(srcslot))
    {
        ORF_SLOT_LOG("tts_optimized_copyslot: Source has no tuple but has values - materializing");
        tts_optimized_materialize(srcslot);
    }
    
    /* Copy the tuple and cache reference */
    if (src_opt->tuple)
    {
        dst_opt->tuple = heap_copytuple(src_opt->tuple);
        dst_opt->cache = src_opt->cache;  /* Share cache reference */
        
        /* 
         * CRITICAL FIX: Don't copy tts_extracted - let destination slot start fresh.
         * The tts_extracted array will be allocated when needed in getsomeattrs().
         * Copying it is unnecessary and dangerous due to potential garbage pointers.
         */
        dst_opt->tts_extracted = NULL;
        
        dst_opt->highest_requested = src_opt->highest_requested;
        
        dstslot->tts_flags |= TTS_FLAG_SHOULDFREE;
        dstslot->tts_flags &= ~TTS_FLAG_EMPTY;
        dstslot->tts_nvalid = srcslot->tts_nvalid;
        
        /* Copy extracted values */
        if (srcslot->tts_nvalid > 0)
        {
            memcpy(dstslot->tts_values, srcslot->tts_values, 
                   srcslot->tts_nvalid * sizeof(Datum));
            memcpy(dstslot->tts_isnull, srcslot->tts_isnull, 
                   srcslot->tts_nvalid * sizeof(bool));
        }
    }
    
    ORF_SLOT_LOG("tts_optimized_copyslot: Copied optimized slot");
}

HeapTuple
tts_optimized_get_heap_tuple(TupleTableSlot *slot)
{
    OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
    
    Assert(!TTS_EMPTY(slot));
    
    /* Materialize if we don't have a tuple */
    if (!opt_slot->tuple)
        tts_optimized_materialize(slot);
        
    return opt_slot->tuple;
}

HeapTuple
tts_optimized_copy_heap_tuple(TupleTableSlot *slot)
{
    OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
    
    Assert(!TTS_EMPTY(slot));
    
    if (!opt_slot->tuple)
        tts_optimized_materialize(slot);
        
    return heap_copytuple(opt_slot->tuple);
}

MinimalTuple
tts_optimized_copy_minimal_tuple(TupleTableSlot *slot)
{
    Assert(!TTS_EMPTY(slot));
    
    /* Extract all attributes if not already done */
    slot_getallattrs(slot);
    
    /* Create minimal tuple from extracted values */
    return heap_form_minimal_tuple(slot->tts_tupleDescriptor,
                                  slot->tts_values,
                                  slot->tts_isnull);
}

/*
 * Utility function to store a tuple in an optimized slot
 */
void
tts_optimized_store_tuple(TupleTableSlot *slot, HeapTuple tuple, 
                         OptimizedColumnMapCache *cache)
{
    OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
    
    /* Clear any existing content */
    tts_optimized_clear(slot);
    
    /* Store the tuple and cache reference */
    opt_slot->tuple = tuple;
    opt_slot->cache = cache;
    
    /* Initialize extraction tracking */
    if (!opt_slot->tts_extracted)
    {
        opt_slot->tts_extracted = (bool *) palloc0(
            slot->tts_tupleDescriptor->natts * sizeof(bool));
    }
    else
    {
        memset(opt_slot->tts_extracted, false, 
               slot->tts_tupleDescriptor->natts * sizeof(bool));
    }
    
    /* Reset tracking state */
    opt_slot->highest_requested = 0;
    
    /* Mark slot as non-empty but no attributes extracted yet */
    slot->tts_flags &= ~TTS_FLAG_EMPTY;
    slot->tts_nvalid = 0;  /* Nothing extracted yet - this is key! */
    
    ORF_SLOT_LOG("tts_optimized_store_tuple: Stored tuple in optimized slot for lazy extraction");
}

/*
 * Create an optimized tuple table slot
 */
TupleTableSlot *
MakeOptimizedTupleTableSlot(TupleDesc tupleDesc, OptimizedColumnMapCache *cache)
{
    OptimizedTupleTableSlot *slot;
    
    slot = (OptimizedTupleTableSlot *) 
        MakeTupleTableSlot(tupleDesc, &TTSOpsOptimizedTuple);
        
    slot->cache = cache;
    
    ORF_SLOT_LOG("MakeOptimizedTupleTableSlot: Created optimized slot with %d attributes", 
                tupleDesc->natts);
    
    return (TupleTableSlot *) slot;
}

/*
 * Debug function to show extraction state
 */
void
tts_optimized_debug_extraction_state(TupleTableSlot *slot)
{
    OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
    int natts = slot->tts_tupleDescriptor->natts;
    int i, extracted_count = 0;
    
    if (!opt_slot->tts_extracted)
    {
        ORF_SLOT_LOG("Debug: No extraction tracking array allocated");
        return;
    }
    
    for (i = 0; i < natts; i++)
    {
        if (opt_slot->tts_extracted[i])
            extracted_count++;
    }
    
    ORF_SLOT_LOG("Debug: Extraction state - %d/%d attributes extracted, tts_nvalid=%d, highest_requested=%d",
                extracted_count, natts, slot->tts_nvalid, opt_slot->highest_requested);
}

#ifdef ORF_SLOT_DEBUG
/*
 * Performance monitoring function
 */
void
tts_optimized_track_access_pattern(TupleTableSlot *slot, int attnum)
{
    static int access_count = 0;
    static int last_attnum = 0;
    
    access_count++;
    
    if (access_count % 100 == 0)  /* Log every 100 accesses */
    {
        ORF_SLOT_LOG("Access pattern: #%d, attnum=%d (prev=%d)", 
                    access_count, attnum, last_attnum);
    }
    
    last_attnum = attnum;
}
#endif

/*
 * Slot operations structure - this is the key interface that PostgreSQL uses
 */
const TupleTableSlotOps TTSOpsOptimizedTuple = {
    .base_slot_size = sizeof(OptimizedTupleTableSlot),
    .init = tts_optimized_init,
    .release = tts_optimized_release,
    .clear = tts_optimized_clear,
    .getsomeattrs = tts_optimized_getsomeattrs,      /* Smart extraction */
    .getsysattr = tts_optimized_getsysattr,
    .materialize = tts_optimized_materialize,
    .copyslot = tts_optimized_copyslot,
    .get_heap_tuple = tts_optimized_get_heap_tuple,
    
    /* We can't directly provide minimal tuples from optimized format */
    .get_minimal_tuple = NULL,
    .copy_heap_tuple = tts_optimized_copy_heap_tuple,
    .copy_minimal_tuple = tts_optimized_copy_minimal_tuple
};
