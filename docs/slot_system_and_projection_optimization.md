# PostgreSQL Slot System and Projection Optimization Strategy

**Date:** September 27, 2025  
**Purpose:** Explain PostgreSQL's TupleTableSlot system and our projection optimization approach  
**Context:** Understanding why custom slots are needed for optimized row format

## Table of Contents
1. [What are PostgreSQL Slots?](#what-are-postgresql-slots)
2. [How Standard Heap Slots Work](#how-standard-heap-slots-work)
3. [Why Our Format Needs Custom Slots](#why-our-format-needs-custom-slots)
4. [Projection Optimization Strategy](#projection-optimization-strategy)
5. [Implementation Decision Tree](#implementation-decision-tree)

---

## What are PostgreSQL Slots?

### **The Problem Slots Solve**

PostgreSQL needs to handle tuples (rows) in many different formats:
- **Heap tuples** on disk pages
- **Minimal tuples** for sorting/hashing  
- **Virtual tuples** with separate Datum arrays
- **Buffer tuples** that reference disk buffers
- **Custom format tuples** (like ours!)

**Without slots:** Every piece of code would need to know about every tuple format → Maintenance nightmare

**With slots:** Uniform interface that abstracts tuple format details → Clean architecture

### **Slot as an Interface**

Think of slots like this analogy:

```
Different Tuple Formats = Different Car Models (Toyota, BMW, Tesla)
TupleTableSlot = Universal Car Interface (steering wheel, pedals, etc.)
Slot Operations = How each car implements the interface
```

**Code using slots doesn't care about the underlying format:**
```c
// This works for ANY tuple format:
Datum value = slot_getattr(slot, column_number, &isnull);

// PostgreSQL automatically calls the right implementation:
// - Heap slot → Extract from sequential layout
// - Our slot → Extract using O(1) cache lookup
```

### **Slot Structure**

```c
typedef struct TupleTableSlot
{
    NodeTag     type;
    uint16      tts_flags;          /* Boolean states */
    AttrNumber  tts_nvalid;         /* # of valid values in tts_values */
    const TupleTableSlotOps *tts_ops; /* Implementation methods */
    TupleDesc   tts_tupleDescriptor; /* Slot's tuple descriptor */
    Datum      *tts_values;         /* Current attribute values */
    bool       *tts_isnull;         /* Current attribute isnull flags */
    ItemPointerData tts_tid;        /* Stored tuple's tid */
    Oid         tts_tableOid;       /* Table the tuple came from */
} TupleTableSlot;
```

**Key insight:** `tts_ops` points to format-specific implementations!

---

## How Standard Heap Slots Work

### **Heap Tuple Layout (Sequential)**
```
Heap Tuple:
┌─────────────────┬──────────┬─────────────────────────────────┐
│ HeapTupleHeader │ NULL     │ User Data (catalog order)      │
│ (MVCC fields)   │ bitmap   │ attr1│attr2│attr3│...│attrN    │
└─────────────────┴──────────┴─────────────────────────────────┘
                              ↑
                         t_hoff points here
```

### **Heap Slot Operations**

```c
const TupleTableSlotOps TTSOpsHeapTuple = {
    .base_slot_size = sizeof(HeapTupleTableSlot),
    .init = tts_heap_init,
    .release = tts_heap_release,
    .clear = tts_heap_clear,
    .getsomeattrs = tts_heap_getsomeattrs,    // ← KEY FUNCTION
    .getsysattr = tts_heap_getsysattr,
    // ... other operations
};
```

### **How `slot_getattr()` Works for Heap**

```c
// When someone calls: slot_getattr(slot, 5, &isnull)

slot_getattr(slot, attnum, &isnull)
{
    if (attnum > slot->tts_nvalid)           // Need more attributes?
        slot_getsomeattrs(slot, attnum);     // Extract 1 through attnum
        
    return slot->tts_values[attnum - 1];     // Return cached value
}

// For heap slots, this calls:
tts_heap_getsomeattrs(slot, natts)
{
    // Walk through tuple sequentially, extracting attributes 1→2→3→...→natts
    slot_deform_heap_tuple(slot, hslot->tuple, &hslot->off, natts);
}
```

### **Heap Extraction Process**

**Example: `SELECT col5 FROM table`**

1. **First access:** `slot_getattr(slot, 5, &isnull)`
2. **Check:** `5 > slot->tts_nvalid` (currently 0) → Need to extract
3. **Extract:** `tts_heap_getsomeattrs(slot, 5)` → Walk through cols 1→2→3→4→5
4. **Cache:** Store all 5 values in `slot->tts_values[]`
5. **Return:** `slot->tts_values[4]` (col5)
6. **Future access:** `slot_getattr(slot, 3, &isnull)` → Return cached `slot->tts_values[2]`

**Performance:**
- **First access to colN:** O(N) - must walk through 1→2→...→N
- **Subsequent access:** O(1) - return cached value
- **Access pattern matters:** Accessing col1 then col100 is expensive

---

## Why Our Format Needs Custom Slots

### **Our Optimized Layout (Segregated)**
```
Optimized Tuple:
┌─────────────────┬──────────┬────────┬─────────────┬─────────────┬─────────────┐
│ HeapTupleHeader │ NULL     │ Var    │ Var Offsets │ Fixed Data  │ Var Data    │
│ (MVCC fields)   │ bitmap   │ Count  │ Array       │ (grouped)   │ (grouped)   │
└─────────────────┴──────────┴────────┴─────────────┴─────────────┴─────────────┘
```

### **The Fundamental Difference**

| Aspect | Heap Format | Our Optimized Format |
|--------|-------------|----------------------|
| **Data Organization** | Sequential (1→2→3→4→5) | Segregated (Fixed + Variable sections) |
| **Access Pattern** | Must walk sequentially | Can jump to any column directly |
| **Cache Strategy** | Per-TupleDesc offsets | Pre-computed column map |
| **Optimal Extraction** | Sequential (`getsomeattrs`) | Random access (`getattr`) |

### **Why Heap's `getsomeattrs` is Wrong for Us**

**Heap Logic:** "To get col5, I must walk through col1→col2→col3→col4→col5"
- **Makes sense** because data is sequential
- **Efficient** because you're walking anyway

**Our Logic:** "To get col5, I can jump directly to it using cache"
- **`getsomeattrs(5)` is wasteful** - why extract col1-4 when you only need col5?
- **Direct access is optimal** - O(1) lookup using pre-computed offsets

### **Example: Why Standard Slots Hurt Us**

**Query:** `SELECT col50 FROM table_with_100_columns`

**Using Heap Slot (TTSOpsHeapTuple):**
```c
slot_getattr(slot, 50, &isnull)
    → slot_getsomeattrs(slot, 50)           // Extract cols 1-50
        → slot_deform_heap_tuple(...)       // Walk sequentially 1→2→...→50
            → return col50
```
**Cost:** O(50) extraction + O(50) memory

**Using Our Format with Heap Slot (Current Problem):**
```c
slot_getattr(slot, 50, &isnull)
    → slot_getsomeattrs(slot, 50)           // Extract cols 1-50 (WASTEFUL!)
        → for (i=1; i<=50; i++)             // 50 separate O(1) extractions
            → optimized_extract_attribute(tuple, i, ...)  // O(1) each
            → return col50
```
**Cost:** O(50) × O(1) = O(50) extraction + O(50) memory (WASTEFUL!)

**Using Our Format with Custom Slot (Proposed):**
```c
slot_getattr(slot, 50, &isnull)
    → tts_optimized_getattr(slot, 50, &isnull)  // Direct extraction
        → optimized_extract_attribute(tuple, 50, ...)  // O(1)
        → return col50
```
**Cost:** O(1) extraction + O(1) memory (OPTIMAL!)

---

## Projection Optimization Strategy

### **Current Problem: Always Extract All Columns**

**Our current implementation:**
```c
// In optimized_scan_getnextslot():
for (i = 0; i < natts; i++)  // ← ALWAYS ALL COLUMNS!
{
    slot->tts_values[i] = optimized_extract_attribute(tuple, i + 1, ...);
}
slot->tts_nvalid = natts;  // Mark all as extracted
```

**Impact:**
- `SELECT id FROM table` → Extracts ALL columns, returns only `id`
- `SELECT id, name FROM table` → Extracts ALL columns, returns only `id, name`
- **Defeats the purpose** of our optimized format!

### **Solution 1: Lazy Extraction (Recommended)**

**Approach:** Don't extract anything upfront, let `slot_getattr()` handle it on-demand

```c
// In optimized_scan_getnextslot():
// DON'T extract anything here - just store tuple reference
slot->tts_tuple = tuple;
slot->tts_nvalid = 0;  // Nothing extracted yet

// Extraction happens later when someone calls:
slot_getattr(slot, column_number, &isnull)
    → Extract ONLY that specific column
    → Cache the result for future access
```

### **Solution 2: Custom Slot Implementation (Advanced)**

**Create custom slot operations that leverage our O(1) access:**

```c
const TupleTableSlotOps TTSOpsOptimizedTuple = {
    .base_slot_size = sizeof(OptimizedTupleTableSlot),
    .getsomeattrs = tts_optimized_getsomeattrs,  // Smart extraction
    .getattr = tts_optimized_getattr,            // Direct O(1) access
    // ... other operations
};

// Smart extraction: Only extract what's not already cached
tts_optimized_getsomeattrs(slot, natts)
{
    for (attnum = 1; attnum <= natts; attnum++) {
        if (!already_extracted[attnum]) {
            // Extract ONLY this attribute using O(1) cache lookup
            slot->tts_values[attnum-1] = optimized_extract_attribute(tuple, attnum, ...);
            already_extracted[attnum] = true;
        }
    }
}
```

### **Performance Comparison**

**Query: `SELECT col10, col50, col90 FROM table_with_100_columns`**

| Implementation | Columns Extracted | Extraction Cost | Memory Usage |
|----------------|-------------------|-----------------|--------------|
| **Current (All upfront)** | All 100 columns | O(100) | O(100) |
| **Heap slot (Sequential)** | Columns 1-90 | O(90) | O(90) |
| **Custom slot (Smart)** | Only 10, 50, 90 | O(3) | O(3) |

**Custom slot provides 30x improvement for this query!**

---

## Implementation Decision Tree

### **Phase 1: Quick Fix (1-2 hours) ✅ COMPLETED**
- **Problem:** Cache thrashing (build/destroy cache per attribute)
- **Solution:** Use `optimized_extract_attribute()` with persistent cache
- **Status:** ✅ Fixed - eliminated major performance bottleneck
- **Impact:** From O(N) per attribute to O(1) per attribute

### **Phase 2: Projection Optimization (Current Decision Point)**

**Option A: Minimal Change (2-4 hours)**
- **Approach:** Modify scan function to not extract all columns upfront
- **Implementation:** Store tuple reference, extract on-demand via existing slot system
- **Pros:** Simple, works with existing slot infrastructure
- **Cons:** Still uses `getsomeattrs` pattern (suboptimal for our format)

**Option B: Custom Slot Implementation (1-2 days)**
- **Approach:** Create `TTSOpsOptimizedTuple` with custom operations
- **Implementation:** Smart extraction that leverages O(1) random access
- **Pros:** Architecturally correct, maximum performance benefits
- **Cons:** More complex, requires understanding slot system

**Option C: Hybrid Approach (Recommended)**
- **Phase 2A:** Implement Option A for immediate improvement
- **Phase 2B:** Implement Option B for optimal performance
- **Benefit:** Incremental improvement with clear migration path

### **Recommendation: Start with Option A**

**Rationale:**
1. **Immediate impact:** Will fix projection issues in benchmark tests
2. **Low risk:** Minimal changes to existing working code  
3. **Learning opportunity:** Understand slot behavior before custom implementation
4. **Migration path:** Can upgrade to custom slots later

**Expected improvement from Option A:**
- Many-column SELECT: 5-10x faster (extract only needed columns)
- Single-column SELECT: 20-50x faster (extract 1 vs 100 columns)

---

## Next Steps

### **Immediate (Option A Implementation)**
1. **Modify `optimized_scan_getnextslot()`:**
   - Don't extract all attributes upfront
   - Store tuple reference for lazy extraction
   - Let PostgreSQL's `slot_getattr()` handle on-demand extraction

2. **Test projection benefits:**
   - `SELECT id FROM many_column_table` should be much faster
   - Benchmark against heap to validate improvements

3. **Validate correctness:**
   - Ensure all query types still work
   - Check that cached extraction works properly

### **Future (Option B Implementation)**
1. **Design custom slot operations**
2. **Implement smart `getsomeattrs` that leverages O(1) access**
3. **Integrate with table AM system**
4. **Performance validation and optimization**

---

## Key Insights

### **Why Slots Matter for Performance**
- **Abstraction layer** that determines how attributes are extracted
- **Wrong slot type** can make optimal storage format perform poorly
- **Right slot type** leverages format-specific optimizations

### **Our Format's Superpower**
- **O(1) random access** to any column using pre-computed cache
- **Segregated storage** enables true projection optimization
- **Cache-friendly** design for analytical workloads

### **The Slot System Enables**
- **Format-specific optimizations** without changing query executor
- **Incremental migration** from basic to advanced implementations  
- **Performance isolation** - slot choice doesn't affect correctness

**Bottom line:** Slots are the key to unlocking our format's performance potential. The current cache fix eliminated the major bottleneck, and projection optimization will provide the final performance boost to make our format significantly faster than heap for analytical queries.
