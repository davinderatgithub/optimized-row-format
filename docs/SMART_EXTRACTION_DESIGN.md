# Smart Attribute Extraction Design Document

## Executive Summary

This document describes the design and implementation of **Smart Attribute Extraction** for the `optimized_row_format` PostgreSQL extension. The goal is to leverage our O(1) random attribute access capability to extract only the columns actually needed by a query, avoiding the PostgreSQL contract violation that causes crashes while maintaining the 5.36x performance gains observed for wide tables.

## Problem Statement

### Current Situation
- **Performance Gains**: Achieved 5.36x speedup for accessing last column in 600-column tables
- **Critical Issue**: Violates PostgreSQL's sequential extraction contract
- **Contract**: PostgreSQL assumes ALL attributes from 1 to `tts_nvalid` are valid
- **Crash Scenario**: `SELECT col1, col5` extracts only col5, but PostgreSQL directly accesses garbage data in col1-4

### Root Cause
The performance gains come from bypassing sequential extraction (1→2→3→4→5) and using direct O(1) access (5 only). However, PostgreSQL's expression evaluator in `execExprInterp.c` directly accesses `slot->tts_values[attnum-1]` without calling `slot_getattr()`, assuming all attributes up to `tts_nvalid` are extracted.

## Design Goals

1. **Safety**: Maintain full PostgreSQL contract compliance - no crashes
2. **Performance**: Preserve O(1) random access benefits - extract only needed columns
3. **Correctness**: Handle all query types (SELECT, INSERT, UPDATE, JOIN, aggregates, etc.)
4. **Maintainability**: Minimal changes to PostgreSQL core, use standard extension APIs

## Architectural Approaches Evaluated

### Approach 1: Modify PostgreSQL Core ❌
**Idea**: Add `ExprState*` parameter to `getsomeattrs` function signature.

**Pros**:
- Clean, direct access to expression information
- Most robust solution

**Cons**:
- Requires modifying core PostgreSQL files (`tuptable.h`, `execTuples.c`, `execExprInterp.c`)
- Hard to maintain across PostgreSQL versions
- Not acceptable for an extension

**Decision**: Rejected - too invasive

### Approach 2: Thread-Local Storage ❌
**Idea**: Use thread-local variable to pass `ExprState` from `execExprInterp.c` to `getsomeattrs`.

**Pros**:
- No function signature changes
- Confined to fewer files

**Cons**:
- Fragile hack relying on global state
- Potential issues with concurrent execution
- Requires modifying `execExprInterp.c`

**Decision**: Rejected - too fragile

### Approach 3: Custom Scan Provider ❌
**Idea**: Use PostgreSQL's custom scan API to inject attribute bitmap into execution.

**Pros**:
- Uses standard PostgreSQL extension API
- No core modifications

**Cons**:
- **Fatal Flaw**: Custom scans are for plan nodes, not table access methods
- Table AM and custom scans operate at different layers
- No direct connection between `CustomScanState` and `TableScanDesc`

**Decision**: Rejected - architectural mismatch

### Approach 4: ExecutorStart Hook with Slot Annotation ✅
**Idea**: Use `ExecutorStart_hook` to analyze the query plan, identify required attributes, and store the bitmap directly in the `TupleTableSlot`.

**Pros**:
- Uses standard hook mechanism
- Works with existing table access method architecture
- Bitmap travels with the tuple data
- No core modifications

**Cons**:
- Requires walking the entire plan tree
- Bitmap must be attached to each slot

**Decision**: **SELECTED** - Best balance of safety, performance, and maintainability

## Selected Architecture: ExecutorStart Hook with Slot Annotation

### High-Level Data Flow

```
1. PLANNING PHASE
   Query → Planner → Plan Tree (with Scan nodes)

2. EXECUTOR INITIALIZATION (ExecutorStart_hook)
   Plan Tree → Walk all nodes → Find Scan nodes on optimized tables
   → Walk targetlist & qual expressions → Extract Var nodes
   → Build Bitmapset of required attributes
   → Store bitmap in a global registry keyed by relation OID

3. EXECUTION PHASE (optimized_scan_getnextslot)
   Fetch tuple → Look up bitmap in registry by relation OID
   → Copy bitmap to OptimizedTupleTableSlot
   → Return slot with bitmap attached

4. ATTRIBUTE EXTRACTION (tts_optimized_getsomeattrs)
   Check if slot has bitmap
   → YES: Extract only attributes in bitmap (SMART EXTRACTION)
   → NO: Extract all attributes up to natts (SAFE FALLBACK)
```

### Component Design

#### 1. Bitmap Registry (orf_hooks.c)

```c
/*
 * Global registry mapping relation OID to attribute bitmaps.
 * Populated during ExecutorStart, accessed during scan.
 */
typedef struct OrfBitmapRegistry
{
    HTAB *relation_bitmaps;  /* Hash table: Oid → Bitmapset* */
    MemoryContext registry_context;
} OrfBitmapRegistry;

static OrfBitmapRegistry *bitmap_registry = NULL;
```

**Functions**:
- `orf_registry_init()`: Initialize hash table in TopMemoryContext
- `orf_registry_store(Oid relid, Bitmapset *attrs)`: Store bitmap for relation
- `orf_registry_lookup(Oid relid)`: Retrieve bitmap for relation
- `orf_registry_clear()`: Clear all bitmaps (called at ExecutorEnd)

#### 2. Plan Walker (orf_hooks.c)

```c
/*
 * Walk the plan tree to find all Scan nodes on optimized tables.
 * For each scan, walk the targetlist and qual to find all Var nodes.
 * Build a bitmap of required attributes and store in registry.
 */
static bool orf_plan_walker(PlanState *planstate, void *context);
static bool orf_expression_walker(Node *node, OrfWalkerContext *context);
```

**Algorithm**:
1. Recursively visit all `PlanState` nodes
2. For each `ScanState`:
   - Check if relation uses `optimized_row_format`
   - Walk `plan->targetlist` and `plan->qual`
   - Collect all `Var` nodes with matching `varno`
   - Build `Bitmapset` of `varattno` values
   - Store in registry: `orf_registry_store(relid, bitmap)`

#### 3. Slot Annotation (orf_scan.c)

```c
/*
 * In optimized_scan_getnextslot(), after fetching a tuple:
 * 1. Look up the bitmap for this relation in the registry
 * 2. Copy the bitmap pointer to the slot
 * 3. The bitmap persists for the entire query execution
 */
if (TTS_IS_OPTIMIZED(slot))
{
    OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
    Oid relid = RelationGetRelid(oscan->rel);
    
    opt_slot->attrs_used = orf_registry_lookup(relid);
    
    tts_optimized_store_tuple(slot, tuple, oscan->column_cache);
    // ... rest of slot initialization
}
```

#### 4. Smart Extraction (orf_slot_ops.c)

```c
/*
 * In tts_optimized_getsomeattrs():
 * Check if slot has a bitmap. If yes, extract only those attributes.
 * If no, fall back to extracting all attributes (safe default).
 */
if (opt_slot->attrs_used != NULL)
{
    /* SMART EXTRACTION: Use bitmap */
    int att = -1;
    while ((att = bms_next_member(opt_slot->attrs_used, att)) >= 0)
    {
        if (att > 0 && att <= natts && !opt_slot->tts_extracted[att-1])
        {
            slot->tts_values[att-1] = optimized_extract_attribute(...);
            opt_slot->tts_extracted[att-1] = true;
        }
    }
}
else
{
    /* SAFE FALLBACK: Extract all attributes */
    for (attnum = slot->tts_nvalid + 1; attnum <= natts; attnum++)
    {
        slot->tts_values[attnum-1] = optimized_extract_attribute(...);
    }
}
```

### Critical Implementation Details

#### Handling `tts_nvalid`

**Problem**: PostgreSQL uses `tts_nvalid` to track the highest extracted attribute. With sparse extraction, we can't simply set `tts_nvalid = natts` because attributes in the middle might not be extracted.

**Solution**: 
- After smart extraction, set `tts_nvalid = natts` (the highest requested)
- Mark extracted attributes in `tts_extracted[]` array
- In `slot_getattr()`, check `tts_extracted[]` before accessing `tts_values[]`
- If an attribute is requested but not extracted, extract it on-demand

#### Memory Management

**Bitmap Lifecycle**:
1. **Created**: During `ExecutorStart_hook` in `CurrentMemoryContext`
2. **Stored**: In registry hash table (lives in `TopMemoryContext`)
3. **Referenced**: By slots during execution (no ownership transfer)
4. **Freed**: During `ExecutorEnd_hook` when registry is cleared

**Important**: Slots only hold pointers to bitmaps, they don't own them.

#### Handling Multiple Scans

**Scenario**: Query has multiple scans on the same relation (e.g., self-join).

**Solution**: Each scan may need different attributes. We have two options:
1. **Union**: Store the union of all required attributes for the relation
2. **Per-Scan**: Key registry by (relid, scanid) pair

**Decision**: Use **Union** approach for simplicity. Slightly over-extracts but still much better than extracting all attributes.

### Edge Cases

#### 1. Whole-Row References (`SELECT *`)
**Detection**: `Var` node with `varattno = 0`
**Handling**: If detected, set `attrs_used = NULL` to force fallback extraction of all attributes

#### 2. System Columns (`ctid`, `xmin`, etc.)
**Detection**: `Var` node with `varattno < 0`
**Handling**: System columns are handled separately by `slot_getsysattr()`, not affected by bitmap

#### 3. Subqueries and CTEs
**Handling**: Plan walker recursively visits all plan nodes, including subquery plans

#### 4. INSERT/UPDATE Operations
**Handling**: These don't go through `optimized_scan_getnextslot()`, so bitmap is NULL and fallback extraction is used (safe)

#### 5. Index Scans
**Current**: Delegated to heap AM
**Future**: If we implement custom index scans, apply same bitmap logic

## Implementation Plan

### Phase 1: Core Infrastructure ✅ (Partially Complete)
- [x] Add `attrs_used` field to `OptimizedTupleTableSlot`
- [x] Create `orf_hooks.c` with `_PG_init` and `_PG_fini`
- [ ] Implement bitmap registry (hash table)
- [ ] Implement `ExecutorStart_hook` and `ExecutorEnd_hook`

### Phase 2: Plan Analysis ✅ (Partially Complete)
- [x] Implement `orf_expression_walker()` to find `Var` nodes
- [x] Implement `orf_plan_walker()` to find Scan nodes
- [ ] Fix plan walker to access correct relation information
- [ ] Handle whole-row references and edge cases

### Phase 3: Slot Annotation
- [ ] Modify `optimized_scan_getnextslot()` to look up and attach bitmap
- [ ] Ensure bitmap is copied/referenced correctly

### Phase 4: Smart Extraction ✅ (Complete but needs fixes)
- [x] Implement bitmap-based extraction in `tts_optimized_getsomeattrs()`
- [ ] Fix syntax errors in current implementation
- [ ] Handle `tts_nvalid` correctly with sparse extraction
- [ ] Add on-demand extraction for missed attributes

### Phase 5: Testing & Validation
- [ ] Unit tests for bitmap registry
- [ ] Regression tests for various query patterns
- [ ] Performance benchmarks (compare with baseline)
- [ ] Crash testing with contract violation scenarios

## Expected Performance Impact

### Wide Tables (600 columns)
- **Baseline (current safe)**: 10.611ms (heap) vs 15.364ms (optimized) = 1.4x slower
- **Target (smart extraction)**: 10.611ms (heap) vs 1.980ms (optimized) = **5.36x faster**
- **Improvement**: Restore the 5.36x speedup we observed with unsafe extraction

### Narrow Tables (30 columns)
- **Baseline (current safe)**: Regression due to overhead
- **Target (smart extraction)**: Should be close to heap performance
- **Note**: For narrow tables with all columns accessed, bitmap overhead may slightly hurt performance

### Projection Queries (`SELECT col600 FROM wide_table`)
- **Maximum Benefit**: Only extract 1 column instead of 600
- **Expected**: Near-optimal performance, close to theoretical 600x improvement

## Testing Strategy

### 1. Correctness Tests
```sql
-- Test 1: Sparse column access
SELECT col1, col5, col10 FROM wide_table;

-- Test 2: Whole-row reference
SELECT * FROM wide_table;

-- Test 3: Expression with multiple columns
SELECT col1 + col5 FROM wide_table WHERE col10 > 100;

-- Test 4: Self-join
SELECT t1.col1, t2.col5 FROM wide_table t1 JOIN wide_table t2 ON t1.id = t2.id;

-- Test 5: Subquery
SELECT col1 FROM (SELECT col1, col5 FROM wide_table) sub WHERE col5 > 0;
```

### 2. Performance Benchmarks
- Compare against heap AM for various column widths (10, 30, 100, 600)
- Test different selectivity (1%, 10%, 100% of columns)
- Measure overhead of bitmap lookup and registry

### 3. Crash Testing
- Run full PostgreSQL regression suite with optimized format as default
- Specifically test scenarios that previously crashed

## Risks and Mitigations

### Risk 1: Bitmap Overhead
**Risk**: Looking up bitmap and iterating through it adds overhead
**Mitigation**: 
- Use efficient hash table for registry
- Cache bitmap pointer in slot (no repeated lookups)
- For narrow tables, fallback is fast

### Risk 2: Incomplete Attribute Coverage
**Risk**: Bitmap might miss some required attributes
**Mitigation**:
- Comprehensive plan walking (targetlist, qual, join conditions)
- On-demand extraction as safety net
- Extensive testing

### Risk 3: Memory Leaks
**Risk**: Bitmaps not freed properly
**Mitigation**:
- Clear registry in `ExecutorEnd_hook`
- Use PostgreSQL's memory context system
- Valgrind testing

## Future Enhancements

### 1. Per-Scan Bitmaps
Instead of union, track bitmaps per scan node for even better precision.

### 2. Dynamic Bitmap Updates
If a query dynamically requests more attributes (rare), update bitmap on-the-fly.

### 3. Bitmap Caching
For repeated queries, cache bitmaps across executions (requires plan cache integration).

### 4. Statistics Integration
Track which columns are frequently accessed together to optimize physical layout.

## Conclusion

The Smart Attribute Extraction design provides a robust, maintainable solution to the PostgreSQL contract violation problem while preserving the significant performance gains of O(1) random attribute access. By using standard PostgreSQL hooks and a global bitmap registry, we avoid invasive core modifications while achieving our performance and safety goals.

The implementation is straightforward, testable, and extensible, making it an ideal solution for production use.
