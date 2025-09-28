/*
 * orf_slot_ops.h
 *
 * Custom TupleTableSlot operations for optimized row format
 * Provides O(1) attribute access using pre-computed column cache
 * Implements projection optimization through smart extraction
 */

#ifndef ORF_SLOT_OPS_H
#define ORF_SLOT_OPS_H

#include "postgres.h"
#include "executor/tuptable.h"
#include "optimized_row_format.h"

/*
 * Note: OptimizedTupleTableSlot is defined in optimized_row_format.h
 * We use the existing definition to avoid conflicts.
 */

/*
 * Custom slot operations - the key interface that PostgreSQL uses
 */
extern const TupleTableSlotOps TTSOpsOptimizedTuple;

/*
 * Slot creation and management functions
 */
extern TupleTableSlot *MakeOptimizedTupleTableSlot(TupleDesc tupleDesc, 
                                                   OptimizedColumnMapCache *cache);

/*
 * Internal slot operation functions - these implement the TupleTableSlotOps interface
 */
extern void tts_optimized_init(TupleTableSlot *slot);
extern void tts_optimized_release(TupleTableSlot *slot);
extern void tts_optimized_clear(TupleTableSlot *slot);
extern void tts_optimized_getsomeattrs(TupleTableSlot *slot, int natts);
extern Datum tts_optimized_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull);
extern void tts_optimized_materialize(TupleTableSlot *slot);
extern void tts_optimized_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot);
extern HeapTuple tts_optimized_get_heap_tuple(TupleTableSlot *slot);
extern HeapTuple tts_optimized_copy_heap_tuple(TupleTableSlot *slot);
extern MinimalTuple tts_optimized_copy_minimal_tuple(TupleTableSlot *slot);

/*
 * Utility functions for slot management
 */
extern void tts_optimized_store_tuple(TupleTableSlot *slot, HeapTuple tuple, 
                                     OptimizedColumnMapCache *cache);

/*
 * Debug and diagnostic functions
 */
extern void tts_optimized_debug_extraction_state(TupleTableSlot *slot);

/*
 * Macros for slot type checking
 */
#define TTS_IS_OPTIMIZED(slot) ((slot)->tts_ops == &TTSOpsOptimizedTuple)

/*
 * Performance monitoring - can be disabled in production
 */
#ifdef ORF_SLOT_DEBUG
extern void tts_optimized_track_access_pattern(TupleTableSlot *slot, int attnum);
#define ORF_SLOT_TRACK_ACCESS(slot, attnum) tts_optimized_track_access_pattern(slot, attnum)
#else
#define ORF_SLOT_TRACK_ACCESS(slot, attnum) do { } while (0)
#endif

#endif /* ORF_SLOT_OPS_H */
