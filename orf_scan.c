#include "postgres.h"
#include "access/heapam.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"
#include "executor/tuptable.h"

#include "optimized_row_format.h"
#include "orf_scan.h"
#include "orf_slot.h"
#include "orf_utils.h"

/*
 * Custom logging for optimized row format extension
 * DISABLED for testing - uncomment to enable debugging
 */
// #define OPTIMIZED_LOG(fmt, ...) \
//     elog(NOTICE, "OPTIMIZED_DEBUG: " fmt, ##__VA_ARGS__)
#define OPTIMIZED_LOG(fmt, ...) do { } while (0)

/* Get the heap AM routine to delegate operations to */
static const TableAmRoutine *
get_heap_am_routine(void)
{
    return GetHeapamTableAmRoutine();
}

/* --- Update scan_begin to use the new embedded structure --- */
TableScanDesc
optimized_scan_begin(Relation rel, Snapshot snapshot,
                    int nkeys, struct ScanKeyData *key,
                    ParallelTableScanDesc pscan, uint32 flags)
{
    OptimizedScanDesc oscan = (OptimizedScanDesc) palloc0(sizeof(OptimizedScanDescData));
    TableScanDesc scan = &oscan->rs_base;
    TupleDesc tupleDesc = RelationGetDescr(rel);

    /* Initialize the base scan descriptor */
    scan->rs_rd = rel;
    scan->rs_snapshot = snapshot;
    scan->rs_nkeys = nkeys;
    scan->rs_key = key;
    scan->rs_parallel = NULL;
    scan->rs_flags = flags;

    /* Initialize our private fields */
    oscan->rel = rel;
    oscan->nblocks = RelationGetNumberOfBlocks(rel);
    oscan->currBlock = 0;
    oscan->currOffset = FirstOffsetNumber;
    oscan->currBuffer = InvalidBuffer;

    /* Build and cache column mapping information for O(1) attribute access */
    if (rel->rd_amcache == NULL)
    {
        rel->rd_amcache = build_column_cache(tupleDesc);
        OPTIMIZED_LOG("optimized_scan_begin: built new cache for relation %s",
                     RelationGetRelationName(rel));
    }
    oscan->column_cache = (OptimizedColumnMapCache *) rel->rd_amcache;

    return scan;
}

/* --- Update scan_end to use the new embedded structure --- */
void
optimized_scan_end(TableScanDesc scan)
{
    OptimizedScanDesc oscan = (OptimizedScanDesc) scan;

    if (BufferIsValid(oscan->currBuffer))
        ReleaseBuffer(oscan->currBuffer);
    pfree(oscan);
}

/* --- Update scan_rescan to use the new embedded structure --- */
void
optimized_scan_rescan(TableScanDesc scan, ScanKey key, bool set_params,
                     bool allow_strat, bool allow_sync, bool allow_pagemode)
{
    OptimizedScanDesc oscan = (OptimizedScanDesc) scan;

    /* Update scan keys */
    scan->rs_nkeys = key ? scan->rs_nkeys : 0;
    scan->rs_key = key;

    /* Reset scan state */
    oscan->currBlock = 0;
    oscan->currOffset = FirstOffsetNumber;
    if (BufferIsValid(oscan->currBuffer))
    {
        ReleaseBuffer(oscan->currBuffer);
        oscan->currBuffer = InvalidBuffer;
    }
}

/* --- Implement minimal direct scan using the new embedded structure --- */
/*
 * optimized_scan_getnextslot
 * 
 * This implementation follows the recommended approach for projection optimization:
 * - It does NOT deform/extract all attributes up front.
 * - It stores the raw tuple in the slot and sets up the slot for on-demand attribute extraction.
 * - The slot's tts_ops should be set to the custom TupleTableSlotOps elsewhere (not here).
 */
bool
optimized_scan_getnextslot(TableScanDesc scan, ScanDirection direction,
                          TupleTableSlot *slot)
{
    OptimizedScanDesc oscan = (OptimizedScanDesc) scan;
    HeapTuple tuple = &oscan->o_ctup;
    Page page;
    OffsetNumber maxOffset;
    Snapshot snapshot = scan->rs_snapshot;

    while (oscan->currBlock < oscan->nblocks)
    {
        /* Read the current block if not already loaded */
        if (!BufferIsValid(oscan->currBuffer))
            oscan->currBuffer = ReadBuffer(oscan->rel, oscan->currBlock);
        LockBuffer(oscan->currBuffer, BUFFER_LOCK_SHARE);
        page = BufferGetPage(oscan->currBuffer);
        maxOffset = PageGetMaxOffsetNumber(page);

        while (oscan->currOffset <= maxOffset)
        {
            ItemId itemId = PageGetItemId(page, oscan->currOffset);
            if (ItemIdIsUsed(itemId))
            {
                /* Build HeapTupleData pointing to the tuple */
                tuple->t_len = ItemIdGetLength(itemId);
                tuple->t_data = (HeapTupleHeader) PageGetItem(page, itemId);
                tuple->t_tableOid = RelationGetRelid(oscan->rel);
                ItemPointerSet(&tuple->t_self, oscan->currBlock, oscan->currOffset);

                if (HeapTupleSatisfiesVisibility(tuple, snapshot, oscan->currBuffer))
                {
                    /*
                    * SLOT-BASED PROJECTION OPTIMIZATION:
                    * Store the raw optimized tuple directly in our custom slot.
                    * No attribute extraction happens here - it's all on-demand!
                    * 
                    * When the executor needs specific attributes, it will call our
                    * custom getsomeattrs function which will extract ONLY the
                    * requested attributes from the optimized format.
                    */
                    
                    // Cast to our custom slot type
                    OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
                    
                    ExecClearTuple(slot);
                    
                    // Store the raw optimized tuple (NO EXTRACTION YET!)
                    opt_slot->opt_tuple = heap_copytuple(tuple);
                    
                    // Pass the column cache to the slot for O(1) attribute access
                    opt_slot->column_cache = oscan->column_cache;
                    
                    // PERFORMANCE OPTIMIZATION: Initialize cached data pointers
                    optimized_slot_init_cache(opt_slot);
                    
                    // Initialize attribute cache tracking
                    if (!opt_slot->attr_cached) {
                        opt_slot->attr_cached = (bool *) palloc0(
                            slot->tts_tupleDescriptor->natts * sizeof(bool));
                    } else {
                        memset(opt_slot->attr_cached, false, 
                            slot->tts_tupleDescriptor->natts * sizeof(bool));
                    }
                    
                    // Mark slot as having data but NO extracted attributes yet
                    slot->tts_flags &= ~TTS_FLAG_EMPTY;
                    slot->tts_flags |= TTS_FLAG_SHOULDFREE;
                    slot->tts_nvalid = 0;  // CRITICAL: No attributes extracted yet!
                    slot->tts_tid = tuple->t_self;
                    slot->tts_tableOid = RelationGetRelid(oscan->rel);

                    /* Advance offset for next call */
                    oscan->currOffset++;
                    LockBuffer(oscan->currBuffer, BUFFER_LOCK_UNLOCK);
                    return true;
                }
            }
            oscan->currOffset++;
        }
        /* Done with this block */
        LockBuffer(oscan->currBuffer, BUFFER_LOCK_UNLOCK);
        ReleaseBuffer(oscan->currBuffer);
        oscan->currBuffer = InvalidBuffer;
        oscan->currBlock++;
        oscan->currOffset = FirstOffsetNumber;
    }
    return false;
}

Size
optimized_parallelscan_estimate(Relation rel)
{
	/* For now, delegate to heap AM since scan is not fully implemented */
	const TableAmRoutine *heap_am = GetHeapamTableAmRoutine();
	return heap_am->parallelscan_estimate(rel);
}


/* --- Index Scan Implementation --- */

IndexFetchTableData *
optimized_index_fetch_begin(Relation rel)
{
	const TableAmRoutine *heap_am = get_heap_am_routine();
	return heap_am->index_fetch_begin(rel);
}

void
optimized_index_fetch_reset(IndexFetchTableData *scan)
{
	const TableAmRoutine *heap_am = get_heap_am_routine();
	heap_am->index_fetch_reset(scan);
}

void
optimized_index_fetch_end(IndexFetchTableData *scan)
{
	const TableAmRoutine *heap_am = get_heap_am_routine();
	heap_am->index_fetch_end(scan);
}

bool
optimized_index_fetch_tuple(struct IndexFetchTableData *scan,
                              ItemPointer tid,
                              Snapshot snapshot,
                              TupleTableSlot *slot,
                              bool *call_again, bool *all_dead)
{
	const TableAmRoutine *heap_am = get_heap_am_routine();
	return heap_am->index_fetch_tuple(scan, tid, snapshot, slot, call_again, all_dead);
}
