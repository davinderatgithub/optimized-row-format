# getsomeattrs vs getattr: Why Our Format Needs Different Logic

**Date:** September 27, 2025  
**Context:** Explaining why PostgreSQL's standard `getsomeattrs` pattern doesn't fit our optimized format

## The Core Question

**PostgreSQL Standard:** `getsomeattrs(slot, 5)` → Extract attributes 1, 2, 3, 4, 5  
**Our Format Capability:** Can extract attribute 5 directly in O(1) time  
**Question:** Should we follow PostgreSQL's pattern or leverage our format's strength?

## Storage Format Comparison

### **Heap Format (Sequential Access)**
```
┌─────────────────────────────────────────────────────────┐
│ attr1 │ attr2 │ attr3 │ attr4 │ attr5 │ ... │ attrN     │
└─────────────────────────────────────────────────────────┘
```
**Access Pattern:** To get attr5, must walk: attr1 → attr2 → attr3 → attr4 → attr5  
**Cost:** O(5) to access attr5  
**`getsomeattrs(5)` Logic:** "Since I'm walking anyway, extract 1-5"  
**Result:** Efficient for heap format ✅

### **Our Optimized Format (Random Access)**
```
┌─────────────────┬─────────────┬─────────────┐
│ Fixed Section   │ Var Offsets │ Var Data    │
│ attr1│attr3│... │ [2][4][...] │ attr2│attr4 │
└─────────────────┴─────────────┴─────────────┘
```
**Access Pattern:** To get attr5, direct lookup: `cache[5] → offset → data`  
**Cost:** O(1) to access attr5  
**`getsomeattrs(5)` Logic:** "Extract 1-5 even though I only need 5"  
**Result:** Wasteful for our format ❌

## The `getsomeattrs` Dilemma

### **Standard PostgreSQL Logic**
```c
// PostgreSQL's slot_getattr() calls:
if (attnum > slot->tts_nvalid)
    slot_getsomeattrs(slot, attnum);  // Extract 1 through attnum

return slot->tts_values[attnum - 1];
```

### **Why This Hurts Our Format**

**Example Query:** `SELECT col50 FROM table_with_100_columns`

**What happens with standard logic:**
1. `slot_getattr(slot, 50, &isnull)` called
2. `50 > slot->tts_nvalid` (0) → Need to extract
3. `slot_getsomeattrs(slot, 50)` called
4. **Our implementation extracts columns 1-50** (49 unnecessary extractions!)
5. Return `slot->tts_values[49]`

**What should happen with our format:**
1. `slot_getattr(slot, 50, &isnull)` called  
2. **Extract only column 50** using O(1) cache lookup
3. Return the value

**Performance Impact:**
- **Standard logic:** 50 extractions for 1 needed column
- **Optimal logic:** 1 extraction for 1 needed column
- **Waste factor:** 50x unnecessary work!

## Our Format's Architectural Advantage

### **Key Insight: We're Like an Array, Heap is Like a Linked List**

```c
// Array access (our format):
value = array[50];  // O(1) - direct access

// Linked list access (heap format):  
current = head;
for (i = 0; i < 50; i++)  // O(N) - must traverse
    current = current->next;
value = current->data;
```

### **Implications for Extraction Strategy**

| Storage Type | Optimal Access Pattern | PostgreSQL Standard | Our Decision |
|--------------|------------------------|-------------------|--------------|
| **Sequential (Heap)** | Extract 1→N sequentially | `getsomeattrs(N)` ✅ | Follow standard |
| **Random Access (Ours)** | Extract only what's needed | `getsomeattrs(N)` ❌ | **Custom approach** |

## Decision Analysis

### **Option 1: Follow PostgreSQL Standard (Current)**
```c
tts_optimized_getsomeattrs(slot, natts)
{
    // Extract attributes 1 through natts
    for (i = 1; i <= natts; i++) {
        slot->tts_values[i-1] = optimized_extract_attribute(tuple, i, ...);
    }
    slot->tts_nvalid = natts;
}
```
**Pros:** Compatible with existing PostgreSQL patterns  
**Cons:** Wastes our format's O(1) random access advantage

### **Option 2: Leverage Our Format's Strength (Proposed)**
```c
tts_optimized_getsomeattrs(slot, natts)
{
    // Only extract attributes that haven't been extracted yet
    for (i = 1; i <= natts; i++) {
        if (!already_extracted[i]) {
            slot->tts_values[i-1] = optimized_extract_attribute(tuple, i, ...);
            already_extracted[i] = true;
        }
    }
    // Update tts_nvalid to highest extracted attribute
    slot->tts_nvalid = max(slot->tts_nvalid, natts);
}
```
**Pros:** Leverages O(1) random access, avoids redundant extraction  
**Cons:** More complex tracking, different from PostgreSQL standard

### **Option 3: Direct getattr Implementation (Advanced)**
```c
// Skip getsomeattrs entirely, implement direct getattr
tts_optimized_getattr(slot, attnum, isnull)
{
    if (!already_extracted[attnum]) {
        slot->tts_values[attnum-1] = optimized_extract_attribute(tuple, attnum, ...);
        already_extracted[attnum] = true;
    }
    return slot->tts_values[attnum-1];
}
```
**Pros:** Perfect match for our format, maximum efficiency  
**Cons:** Requires custom slot implementation

## Performance Modeling

### **Query: `SELECT col10, col50, col90 FROM table_with_100_columns`**

**Option 1 (Standard getsomeattrs):**
- First access (col10): Extract cols 1-10 → 10 extractions
- Second access (col50): Extract cols 11-50 → 40 extractions  
- Third access (col90): Extract cols 51-90 → 40 extractions
- **Total:** 90 extractions for 3 needed columns

**Option 2 (Smart getsomeattrs):**
- First access (col10): Extract only col10 → 1 extraction
- Second access (col50): Extract only col50 → 1 extraction
- Third access (col90): Extract only col90 → 1 extraction  
- **Total:** 3 extractions for 3 needed columns

**Option 3 (Direct getattr):**
- Same as Option 2, but with cleaner implementation
- **Total:** 3 extractions for 3 needed columns

**Performance Improvement:** 30x reduction in extraction work!

## Recommendation

### **Phase 1: Smart getsomeattrs (Option 2)**
**Rationale:**
- Works within existing PostgreSQL slot framework
- Provides significant performance improvement
- Maintains compatibility with existing code
- Can be implemented incrementally

### **Phase 2: Custom Slot with Direct getattr (Option 3)**  
**Rationale:**
- Architecturally correct solution
- Maximum performance benefits
- Clean separation of concerns
- Future-proof design

## Implementation Priority

### **Immediate (Current Sprint)**
1. **Fix cache thrashing** ✅ DONE
2. **Implement smart getsomeattrs** (Option 2)
3. **Validate projection performance improvements**

### **Future (Next Sprint)**
1. **Design custom slot operations** (Option 3)
2. **Implement direct getattr pattern**
3. **Performance optimization and tuning**

## Key Takeaways

### **Why This Matters**
- **Format-specific optimization:** Different storage formats need different access patterns
- **Performance impact:** Wrong pattern can make optimal format perform poorly
- **Architectural insight:** Our format is fundamentally different from heap

### **The Decision**
- **Don't blindly follow PostgreSQL patterns** when they don't fit our format
- **Leverage our format's strengths** (O(1) random access) instead of working against them
- **Implement smart extraction** that avoids unnecessary work

### **Expected Results**
- **Projection queries:** 10-50x faster (extract only needed columns)
- **Analytical workloads:** Significant improvement for column-subset queries
- **Memory efficiency:** Reduced memory usage for selective queries

**Bottom Line:** Our optimized format's random access capability requires a different extraction strategy than heap's sequential access. The `getsomeattrs` pattern should be adapted to leverage our format's strengths, not force it into heap's limitations.
