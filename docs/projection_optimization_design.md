# Projection Optimization Design for Optimized Row Format

**Date:** September 27, 2025  
**Version:** 1.0  
**Status:** Design Phase  

## Executive Summary

This document outlines the design for implementing projection optimization in the optimized row format extension. The current implementation extracts all columns for every query, defeating the purpose of our O(1) random access capability. This design proposes a custom slot-based solution that leverages our format's architectural advantages to provide true projection optimization.

**Expected Impact:** 10-100x performance improvement for analytical queries that select subsets of columns.

---

## Table of Contents

1. [Problem Statement](#problem-statement)
2. [Current Architecture Analysis](#current-architecture-analysis)
3. [PostgreSQL Slot System Overview](#postgresql-slot-system-overview)
4. [Design Rationale](#design-rationale)
5. [Proposed Solution](#proposed-solution)
6. [Implementation Plan](#implementation-plan)
7. [Performance Analysis](#performance-analysis)
8. [Risk Assessment](#risk-assessment)

---

## Problem Statement

### **Current Performance Issue**

Our optimized row format provides O(1) random access to any column using pre-computed cache, but we're not leveraging this advantage:

```sql
-- Query that should be 50x faster than heap:
SELECT col50 FROM table_with_100_columns;

-- Current behavior: Extract ALL 100 columns, return col50
-- Optimal behavior: Extract ONLY col50, return col50
```

**Root Cause:** We extract all attributes upfront in the scan function, bypassing PostgreSQL's lazy extraction mechanism.

### **Why This Matters**

**Analytical Workloads:** Typically select 5-20% of available columns
- `SELECT customer_id, order_date FROM orders` (2 of 50 columns)
- `SELECT product_name, price FROM products WHERE category = 'electronics'` (2 of 30 columns)

**Current Waste:** 80-95% of extraction work is unnecessary

---

## Current Architecture Analysis

### **How We've Been Working Without Custom Slots**

**Current Scan Function Logic:**
```c
// In optimized_scan_getnextslot():
for (i = 0; i < natts; i++)  // ← Extract ALL columns
{
    slot->tts_values[i] = optimized_extract_attribute(tuple, i + 1, ...);
}
slot->tts_nvalid = natts;  // ← Mark ALL as extracted
```

**Why This Works:**
1. **Bypass slot system entirely** - directly populate `slot->tts_values[]`
2. **Mark all attributes as extracted** - `slot->tts_nvalid = natts`
3. **PostgreSQL never calls slot operations** - just reads pre-extracted values

**PostgreSQL's slot_getattr() Flow:**
```c
slot_getattr(slot, attnum, &isnull)
{
    if (attnum > slot->tts_nvalid)        // Is attribute extracted?
        slot_getsomeattrs(slot, attnum);  // No → Extract it
    
    return slot->tts_values[attnum - 1];  // Return pre-extracted value
}
```

**Since `slot->tts_nvalid = natts` (all attributes), the condition is NEVER true → PostgreSQL never calls slot operations.**

### **Current Performance Characteristics**

| Query Type | Columns Needed | Columns Extracted | Efficiency |
|------------|----------------|-------------------|------------|
| `SELECT *` | 100 | 100 | 100% ✅ |
| `SELECT col1, col2` | 2 | 100 | 2% ❌ |
| `SELECT col50` | 1 | 100 | 1% ❌ |

---

## PostgreSQL Slot System Overview

### **What Are Slots?**

**Problem Slots Solve:** PostgreSQL handles many tuple formats (heap, minimal, virtual, custom)  
**Solution:** Uniform interface that abstracts format-specific details

**Analogy:**
```
Different Tuple Formats = Different Car Models (Toyota, BMW, Tesla)
TupleTableSlot = Universal Car Interface (steering wheel, pedals)
Slot Operations = How each car implements the interface
```

### **Slot Structure**
```c
typedef struct TupleTableSlot
{
    uint16      tts_flags;          /* Boolean states */
    AttrNumber  tts_nvalid;         /* # of valid values in tts_values */
    const TupleTableSlotOps *tts_ops; /* ← Implementation methods */
    TupleDesc   tts_tupleDescriptor; /* Slot's tuple descriptor */
    Datum      *tts_values;         /* Current attribute values */
    bool       *tts_isnull;         /* Current attribute isnull flags */
    // ... other fields
} TupleTableSlot;
```

**Key Insight:** `tts_ops` points to format-specific implementations!

### **Standard Heap Slot Operations**

```c
const TupleTableSlotOps TTSOpsHeapTuple = {
    .base_slot_size = sizeof(HeapTupleTableSlot),
    .getsomeattrs = tts_heap_getsomeattrs,    // ← Sequential extraction
    .getsysattr = tts_heap_getsysattr,
    // ... other operations
};
```

**Heap Extraction Logic:**
```c
tts_heap_getsomeattrs(slot, natts)
{
    // Walk through tuple sequentially: attr1 → attr2 → ... → attrN
    slot_deform_heap_tuple(slot, hslot->tuple, &hslot->off, natts);
}
```

---

## Design Rationale

### **Storage Format Comparison**

#### **Heap Format (Sequential Access)**
```
┌─────────────────────────────────────────────────────────┐
│ attr1 │ attr2 │ attr3 │ attr4 │ attr5 │ ... │ attrN     │
└─────────────────────────────────────────────────────────┘
```
**Access Pattern:** To get attr5, must walk: attr1 → attr2 → attr3 → attr4 → attr5  
**Cost:** O(5) to access attr5  
**`getsomeattrs(5)` Logic:** "Since I'm walking anyway, extract 1-5" ✅ **Efficient**

#### **Our Optimized Format (Random Access)**
```
┌─────────────────┬─────────────┬─────────────┐
│ Fixed Section   │ Var Offsets │ Var Data    │
│ attr1│attr3│... │ [2][4][...] │ attr2│attr4 │
└─────────────────┴─────────────┴─────────────┘
```
**Access Pattern:** To get attr5, direct lookup: `cache[5] → offset → data`  
**Cost:** O(1) to access attr5  
**`getsomeattrs(5)` Logic:** "Extract 1-5 even though I only need 5" ❌ **Wasteful**

### **Architectural Insight: Array vs Linked List**

```c
// Array access (our format):
value = array[50];  // O(1) - direct access

// Linked list access (heap format):  
current = head;
for (i = 0; i < 50; i++)  // O(N) - must traverse
    current = current->next;
value = current->data;
```

**Implication:** Different storage architectures require different extraction strategies.

### **Why Standard Slots Don't Work for Projection**

**Problem:** If we change to lazy extraction without custom slots:

```c
// In optimized_scan_getnextslot():
slot->tts_nvalid = 0;  // Nothing extracted yet
```

**What happens:**
```c
slot_getattr(slot, 1, &isnull)
    → 1 > 0? Yes → slot_getsomeattrs(slot, 1)
        → Calls tts_heap_getsomeattrs()  // ← WRONG! Uses heap logic
            → slot_deform_heap_tuple()   // ← CRASH! Expects heap format
```

**Result:** Crash or incorrect data because standard slots don't understand our format.

---

## Proposed Solution

### **Custom Slot Implementation**

**Core Idea:** Create `TTSOpsOptimizedTuple` that understands our format and leverages O(1) random access.

#### **Custom Slot Structure**
```c
typedef struct OptimizedTupleTableSlot
{
    TupleTableSlot base;                    /* Base slot structure */
    HeapTuple tuple;                       /* Pointer to optimized tuple */
    OptimizedColumnMapCache *cache;        /* Reference to relation cache */
    bool *tts_extracted;                   /* Per-attribute extraction flags */
} OptimizedTupleTableSlot;
```

#### **Custom Slot Operations**
```c
const TupleTableSlotOps TTSOpsOptimizedTuple = {
    .base_slot_size = sizeof(OptimizedTupleTableSlot),
    .init = tts_optimized_init,
    .release = tts_optimized_release,
    .clear = tts_optimized_clear,
    .getsomeattrs = tts_optimized_getsomeattrs,  // ← Smart extraction
    .getsysattr = tts_optimized_getsysattr,
    .materialize = tts_optimized_materialize,
    // ... other operations
};
```

### **Smart Extraction Logic**

#### **Key Innovation: Non-Sequential Extraction**

Unlike heap slots that extract sequentially (1→2→3→4→5), our slots can extract any combination:

```c
void tts_optimized_getsomeattrs(TupleTableSlot *slot, int natts)
{
    OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
    
    // Only extract attributes that haven't been extracted yet
    for (int attnum = 1; attnum <= natts; attnum++) {
        if (!opt_slot->tts_extracted[attnum - 1]) {
            // Extract ONLY this attribute using O(1) cache lookup
            slot->tts_values[attnum - 1] = optimized_extract_attribute(
                opt_slot->tuple, attnum, slot->tts_tupleDescriptor,
                opt_slot->cache, &slot->tts_isnull[attnum - 1]
            );
            opt_slot->tts_extracted[attnum - 1] = true;
        }
    }
    
    // Update tts_nvalid to highest extracted attribute
    if (natts > slot->tts_nvalid)
        slot->tts_nvalid = natts;
}
```

#### **Advanced: Direct getattr Implementation**
```c
// Skip getsomeattrs entirely for maximum efficiency
Datum tts_optimized_getattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
    OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
    
    // Check if already extracted
    if (attnum <= slot->tts_nvalid && opt_slot->tts_extracted[attnum - 1]) {
        *isnull = slot->tts_isnull[attnum - 1];
        return slot->tts_values[attnum - 1];
    }
    
    // Extract ONLY this specific attribute
    Datum value = optimized_extract_attribute(
        opt_slot->tuple, attnum, slot->tts_tupleDescriptor,
        opt_slot->cache, isnull
    );
    
    // Cache the result
    slot->tts_values[attnum - 1] = value;
    slot->tts_isnull[attnum - 1] = *isnull;
    opt_slot->tts_extracted[attnum - 1] = true;
    
    return value;
}
```

### **Integration with Scan Function**

#### **Modified Scan Logic**
```c
bool optimized_scan_getnextslot(TableScanDesc scan, ScanDirection direction,
                               TupleTableSlot *slot)
{
    // ... find visible tuple ...
    
    if (slot->tts_ops == &TTSOpsOptimizedTuple) {
        // Use custom slot for optimal performance
        OptimizedTupleTableSlot *opt_slot = (OptimizedTupleTableSlot *) slot;
        
        // Store tuple and cache reference - DON'T extract anything
        opt_slot->tuple = tuple;
        opt_slot->cache = oscan->column_cache;
        
        // Reset extraction tracking
        memset(opt_slot->tts_extracted, false, 
               slot->tts_tupleDescriptor->natts * sizeof(bool));
        
        // Mark slot as non-empty but no attributes extracted yet
        slot->tts_flags &= ~TTS_FLAG_EMPTY;
        slot->tts_nvalid = 0;  // ← Nothing extracted yet!
        slot->tts_tid = tuple->t_self;
        slot->tts_tableOid = RelationGetRelid(oscan->rel);
        
        return true;
    } else {
        // Fallback for non-optimized slots - extract all attributes
        // ... current implementation ...
    }
}
```

---

## Implementation Plan

### **Phase 1: Foundation (Week 1)**

#### **1.1 Custom Slot Structure**
- [ ] Define `OptimizedTupleTableSlot` structure
- [ ] Implement basic slot operations (init, release, clear)
- [ ] Create slot creation function `MakeOptimizedTupleTableSlot()`

#### **1.2 Smart Extraction Logic**
- [ ] Implement `tts_optimized_getsomeattrs()` with non-sequential extraction
- [ ] Add extraction tracking with `tts_extracted` array
- [ ] Handle edge cases (system attributes, NULL values)

#### **1.3 Integration**
- [ ] Modify scan function to use custom slots when available
- [ ] Maintain fallback for standard slots
- [ ] Update table AM registration to use custom slots

### **Phase 2: Optimization (Week 2)**

#### **2.1 Direct getattr Implementation**
- [ ] Implement `tts_optimized_getattr()` for maximum efficiency
- [ ] Bypass `getsomeattrs` for single-attribute access
- [ ] Optimize for common access patterns

#### **2.2 Advanced Features**
- [ ] Implement materialization support
- [ ] Add slot copying operations
- [ ] Support for minimal tuple conversion

#### **2.3 Performance Tuning**
- [ ] Optimize extraction tracking overhead
- [ ] Cache-friendly data structures
- [ ] Memory management optimization

### **Phase 3: Validation (Week 3)**

#### **3.1 Correctness Testing**
- [ ] Comprehensive regression tests
- [ ] Edge case validation
- [ ] Multi-column query testing

#### **3.2 Performance Validation**
- [ ] Benchmark against heap format
- [ ] Measure projection optimization benefits
- [ ] Profile memory usage

#### **3.3 Production Readiness**
- [ ] Error handling and recovery
- [ ] Documentation and examples
- [ ] Performance tuning guidelines

---

## Performance Analysis

### **Expected Performance Improvements**

#### **Projection Queries**
| Query Type | Columns Needed | Current Extraction | Optimized Extraction | Improvement |
|------------|----------------|-------------------|---------------------|-------------|
| `SELECT col1` | 1 | 100 | 1 | 100x |
| `SELECT col1, col50` | 2 | 100 | 2 | 50x |
| `SELECT col10, col50, col90` | 3 | 100 | 3 | 33x |
| `SELECT *` | 100 | 100 | 100 | 1x (no change) |

#### **Memory Usage**
- **Current:** Always allocate space for all columns
- **Optimized:** Allocate only for accessed columns
- **Benefit:** 50-95% memory reduction for selective queries

#### **Cache Performance**
- **Current:** Extract all columns → Poor cache locality
- **Optimized:** Extract only needed columns → Better cache locality
- **Benefit:** Improved CPU cache utilization

### **Performance Modeling**

#### **Analytical Workload Simulation**
```
Table: 100 columns, 1M rows
Query Mix:
- 60% single-column queries (SELECT col_x)
- 30% few-column queries (SELECT col_a, col_b, col_c)  
- 10% full-table queries (SELECT *)

Expected Overall Improvement: 20-40x for analytical workloads
```

#### **OLTP Workload Impact**
```
Table: 20 columns, typical CRUD operations
Query Mix:
- 70% multi-column queries (SELECT col1, col2, ..., col8)
- 30% full-row queries (SELECT *)

Expected Overall Improvement: 2-3x for OLTP workloads
```

---

## Risk Assessment

### **Technical Risks**

#### **High Risk**
- **Slot System Complexity:** Custom slots interact with many PostgreSQL subsystems
  - **Mitigation:** Incremental implementation with extensive testing
  - **Fallback:** Maintain compatibility with standard slots

#### **Medium Risk**
- **Memory Management:** Custom slots need proper memory lifecycle management
  - **Mitigation:** Follow PostgreSQL memory context patterns
  - **Testing:** Extensive memory leak testing

- **Query Executor Integration:** Ensure compatibility with all query types
  - **Mitigation:** Comprehensive regression testing
  - **Validation:** Test with complex queries (joins, subqueries, etc.)

#### **Low Risk**
- **Performance Regression:** Risk of making simple queries slower
  - **Mitigation:** Benchmark-driven development
  - **Monitoring:** Continuous performance testing

### **Implementation Risks**

#### **Complexity Risk**
- **Issue:** Custom slots add significant complexity
- **Mitigation:** 
  - Start with minimal viable implementation
  - Extensive documentation and comments
  - Code review process

#### **Maintenance Risk**
- **Issue:** Custom code may break with PostgreSQL updates
- **Mitigation:**
  - Follow PostgreSQL coding standards
  - Use stable APIs where possible
  - Maintain compatibility layer

### **Rollback Strategy**

If custom slots prove problematic:
1. **Immediate:** Disable custom slots, fall back to current implementation
2. **Short-term:** Implement simpler projection optimization without custom slots
3. **Long-term:** Redesign approach based on lessons learned

---

## Success Metrics

### **Performance Targets**
- **Projection queries:** 10-50x improvement over current implementation
- **Full-table queries:** No performance regression
- **Memory usage:** 50-90% reduction for selective queries

### **Quality Targets**
- **Correctness:** Pass all existing regression tests
- **Stability:** No crashes or data corruption
- **Compatibility:** Work with all PostgreSQL query types

### **Adoption Metrics**
- **Benchmark results:** Demonstrate clear advantages in performance tests
- **Real-world validation:** Test with actual analytical workloads
- **Community feedback:** Positive reception from PostgreSQL community

---

## Conclusion

The projection optimization design leverages our optimized row format's key architectural advantage: O(1) random access to any column. By implementing custom PostgreSQL slots that understand our format, we can provide true projection optimization that dramatically improves performance for analytical workloads.

**Key Benefits:**
1. **10-100x performance improvement** for column-subset queries
2. **Significant memory reduction** for selective queries  
3. **Architectural correctness** - proper integration with PostgreSQL's slot system
4. **Future-proof design** - enables further optimizations

**Implementation Strategy:**
- **Incremental approach** with fallback to current implementation
- **Extensive testing** to ensure correctness and stability
- **Performance-driven development** with continuous benchmarking

This design positions our optimized row format as a compelling alternative to standard heap storage for analytical and mixed workloads, while maintaining full compatibility with PostgreSQL's ecosystem.
