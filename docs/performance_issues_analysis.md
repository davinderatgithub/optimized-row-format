# Performance Issues Analysis - Optimized Row Format Extension

**Date:** September 27, 2025  
**Status:** Critical Performance Issues Identified  
**Priority:** High - These issues are making the optimized format slower than standard heap

## Executive Summary

The optimized row format extension has fundamental performance issues in its SELECT query implementation that completely negate the benefits of the optimized storage layout. The current implementation is likely **slower than standard heap** due to cache thrashing and unnecessary attribute extraction.

## Critical Issues Identified

### 1. **Cache Thrashing in Attribute Extraction**

**Issue:** `optimized_extract_attribute_no_cache()` creates and destroys cache for every single attribute extraction.

**Code Location:** `orf_utils.c:104-119`

```c
optimized_extract_attribute_no_cache(HeapTuple tuple, int attnum, TupleDesc tupleDesc, bool *isnull)
{
    /* Build a temporary cache for this operation */
    OptimizedColumnMapCache *temp_cache = build_column_cache(tupleDesc);  // ❌ EXPENSIVE!
    
    result = optimized_extract_attribute(tuple, attnum, tupleDesc, temp_cache, isnull);
    
    /* Clean up temporary cache */
    pfree(temp_cache->fixed_offsets);  // ❌ WASTEFUL!
    pfree(temp_cache->var_indexes);
    pfree(temp_cache);
    
    return result;
}
```

**Impact:**
- For a query selecting 5 columns from 10,000 rows = **50,000 cache builds/destroys**
- Massive memory allocation overhead
- Complete negation of O(1) access benefits
- Likely slower than heap's cached offset approach

**Root Cause:** Cache scope is per-attribute instead of per-relation

### 2. **No Column Projection Optimization**

**Issue:** Always extracts ALL columns even when query only needs a subset.

**Code Location:** `orf_scan.c:169-175`

```c
// Extract all attributes from optimized format and store directly in slot
for (i = 0; i < natts; i++)  // ❌ ALWAYS ALL COLUMNS!
{
    ORF_DEBUG_VERBOSE(scan, "Extracting attribute %d", i);
    slot->tts_values[i] = optimized_extract_attribute_no_cache(tuple, i + 1, tupdesc, &slot->tts_isnull[i]);
    ORF_DEBUG_VERBOSE(scan, "Extracted attribute %d successfully", i);
}
```

**Impact:**
- `SELECT id, name FROM table` extracts ALL columns, not just `id` and `name`
- Defeats the primary benefit of columnar-style organization
- Unnecessary CPU and memory overhead
- Missing projection optimization opportunity

**Root Cause:** Simplified implementation that doesn't leverage PostgreSQL's projection capabilities

### 3. **Missing Cached Attribute Access Path**

**Issue:** No direct path to use cached attribute extraction without rebuilding cache.

**Current Call Chain:**
```
optimized_scan_getnextslot() 
    → optimized_extract_attribute_no_cache()  // ❌ Always no-cache version
        → build_column_cache()                // ❌ Build cache
        → optimized_extract_attribute()       // ✅ Use cache  
        → pfree(cache)                        // ❌ Destroy cache
```

**Expected Call Chain:**
```
optimized_scan_getnextslot() 
    → optimized_extract_attribute()           // ✅ Use persistent cache
        → [O(1) access using cached offsets]  // ✅ Fast path
```

**Impact:**
- The cached version `optimized_extract_attribute()` exists but is never used directly
- Cache is always rebuilt instead of reused
- No performance benefit from cache infrastructure

### 4. **Incorrect Cache Storage Strategy**

**Issue:** Cache architecture doesn't follow PostgreSQL patterns for persistent caching.

**Current Strategy:**
- Cache built per-tuple and immediately destroyed
- No reuse across tuples of the same relation
- Stored in temporary variables

**PostgreSQL Heap Strategy:**
- Cache stored in `TupleDesc->attrs[i]->attcacheoff`
- Persistent across all tuples of same relation
- One-time calculation, infinite reuse

**Impact:**
- Missing the fundamental performance optimization pattern used by heap
- Constant memory allocation/deallocation overhead
- No learning/optimization across multiple tuple accesses

## Performance Comparison Analysis

### Standard Heap Attribute Access
```c
fastgetattr(HeapTuple tup, int attnum, TupleDesc tupleDesc, bool *isnull)
{
    att = TupleDescAttr(tupleDesc, attnum - 1);
    if (att->attcacheoff >= 0)  // ✅ CACHED OFFSET - O(1)
        return fetchatt(att, tup->t_data + tup->t_data->t_hoff + att->attcacheoff);
    else
        return nocachegetattr(tup, attnum, tupleDesc);  // Calculate once, cache forever
}
```

### Current Optimized Format (Broken)
```c
// Called for EVERY attribute of EVERY tuple
optimized_extract_attribute_no_cache()
{
    temp_cache = build_column_cache(tupleDesc);  // ❌ O(N) every time
    result = optimized_extract_attribute(...);   // ✅ O(1) but cache is temporary
    pfree(temp_cache);                          // ❌ Throw away work
}
```

## Call Stack Analysis

### Heap Cached Access Path
```
heap_getattr() → fastgetattr() → [cached offset lookup] → fetchatt()
```
**Characteristics:** O(1) after first access, minimal overhead

### Current Optimized Format Path  
```
optimized_scan_getnextslot() → optimized_extract_attribute_no_cache() → 
build_column_cache() → optimized_extract_attribute() → pfree(cache)
```
**Characteristics:** O(N) every time, maximum overhead

## Architectural Issues

### 1. **Cache Scope Mismatch**
- **Heap:** Caches in `TupleDesc` (shared across all tuples)
- **Optimized:** Caches per-tuple (no sharing)

### 2. **Missing Projection Integration**
- **Heap:** Uses PostgreSQL's slot system for on-demand attribute access
- **Optimized:** Always extracts all attributes upfront

### 3. **Cache Lifecycle Management**
- **Heap:** Cache persists for lifetime of `TupleDesc`
- **Optimized:** Cache destroyed immediately after use

## Impact Assessment

### Performance Impact
- **Current implementation likely 5-10x slower than heap** due to cache thrashing
- **Memory allocation overhead** dominates any storage layout benefits
- **No projection benefits** despite columnar-style organization

### Correctness Impact
- Functionally correct but performance-wise counterproductive
- All MVCC and data integrity aspects work properly
- Storage format is sound, only access patterns are problematic

## Recommended Solutions (High Level)

### 1. **Implement Persistent Caching**
- Store cache in `Relation->rd_amcache` (already partially implemented)
- Use cached version directly without rebuilding
- Follow PostgreSQL's caching patterns

### 2. **Add Column Projection Support**
- Only extract attributes that are actually needed by the query
- Integrate with PostgreSQL's slot system properly
- Implement lazy attribute extraction

### 3. **Optimize Cache Usage**
- Direct path to cached attribute extraction
- Eliminate cache build/destroy cycle
- Reuse cache across multiple tuples

### 4. **Performance Testing Framework**
- Benchmark against heap with identical queries
- Measure cache hit rates and allocation overhead
- Validate projection optimization benefits

## Next Steps

1. **Fix cache thrashing** - highest priority, biggest impact
2. **Implement projection optimization** - leverage columnar benefits
3. **Performance benchmarking** - validate improvements
4. **Integration testing** - ensure no regressions in correctness

## Files Requiring Changes

- `orf_scan.c` - Fix scan implementation to use persistent cache
- `orf_utils.c` - Eliminate cache thrashing in attribute extraction
- `orf_slot.c` - Implement proper slot integration with projection
- Performance test suite - Add benchmarks to prevent regressions

---

**Note:** These issues explain why the optimized format may not be showing expected performance improvements in SELECT queries. The storage format optimization is sound, but the access patterns need to be fixed to realize the benefits.
