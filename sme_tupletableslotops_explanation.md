# TupleTableSlotOps in PostgreSQL Table Access Methods

**SME Technical Guide**: sme_01_postgres_expert  
**Date**: 2025-08-09  
**Topic**: Understanding TupleTableSlotOps in Table Access Method Development

## Overview

`TupleTableSlotOps` is the **critical interface** between PostgreSQL's executor and different tuple storage formats. It's a structure containing function pointers that define how tuples are accessed, materialized, and manipulated.

## Core Architecture

### Purpose
- **Abstraction Layer**: Allows executor to work with any storage format uniformly
- **Performance Optimization**: Enables format-specific optimizations (like projection)
- **Extensibility**: Permits custom Table AMs to define their own tuple operations

### Key Structure
```c
struct TupleTableSlotOps {
    size_t base_slot_size;           // Memory size for slot
    void (*init)(TupleTableSlot *);  // Initialize slot
    void (*release)(TupleTableSlot *); // Cleanup resources
    void (*clear)(TupleTableSlot *);   // Clear slot contents
    void (*getsomeattrs)(TupleTableSlot *, int natts);  // CRITICAL: Fetch attributes
    Datum (*getsysattr)(TupleTableSlot *, int, bool *); // System attributes
    // ... additional callbacks
};
```

## Critical Callback: getsomeattrs()

### Purpose
The `getsomeattrs()` function is **THE MOST IMPORTANT** callback for performance:
- Called when executor needs specific attributes from a tuple
- Should implement **lazy/on-demand** attribute fetching
- Enables **projection optimization** (only fetch needed columns)

### Performance Impact
**WRONG Implementation** (causes 1000x regression):
```c
void bad_getsomeattrs(TupleTableSlot *slot, int natts) {
    // BAD: Fetches ALL attributes every time
    for (int i = 1; i <= natts; i++) {
        slot->tts_values[i-1] = extract_attribute(slot, i, &slot->tts_isnull[i-1]);
    }
}
```

**CORRECT Implementation**:
```c
void good_getsomeattrs(TupleTableSlot *slot, int natts) {
    // GOOD: Only fetch attributes not already fetched
    for (int i = slot->tts_nvalid + 1; i <= natts; i++) {
        slot->tts_values[i-1] = extract_attribute(slot, i, &slot->tts_isnull[i-1]);
    }
    slot->tts_nvalid = natts;
}
```

## Built-in TupleTableSlotOps Types

1. **`TTSOpsVirtual`**: For virtual tuples (Datum/isnull arrays)
2. **`TTSOpsHeapTuple`**: For standard heap tuples in memory
3. **`TTSOpsBufferHeapTuple`**: For heap tuples in shared buffers
4. **`TTSOpsMinimalTuple`**: For minimal tuple format

## Table AM Integration

### How Table AMs Use TupleTableSlotOps

1. **Define Custom Operations**: Create custom `TupleTableSlotOps` for your format
2. **Return from slot_callbacks()**: Table AM's `slot_callbacks()` returns your ops
3. **Executor Uses Operations**: Executor calls your functions via function pointers

### Example Integration
```c
// In your Table AM
static const TupleTableSlotOps MyCustomOps = {
    .base_slot_size = sizeof(HeapTupleTableSlot),
    .init = my_slot_init,
    .release = my_slot_release,
    .clear = my_slot_clear,
    .getsomeattrs = my_getsomeattrs,  // YOUR PERFORMANCE-CRITICAL FUNCTION
    .getsysattr = my_getsysattr,
    // ... other callbacks
};

// Return your ops from slot_callbacks
const TupleTableSlotOps *
my_slot_callbacks(Relation rel) {
    return &MyCustomOps;
}
```

## Critical Execution Flow

### When SELECT Executes:
1. **Executor** calls `slot_getsomeattrs(slot, needed_attrs)`
2. **Framework** checks if `slot->tts_nvalid < needed_attrs`
3. **If true**: Calls your `getsomeattrs()` implementation
4. **Your function** fetches only the needed attributes efficiently
5. **Executor** accesses `slot->tts_values[]` and `slot->tts_isnull[]` arrays

### State Management
- **`tts_nvalid`**: Number of attributes currently valid in arrays
- **`tts_values[]`**: Datum array containing attribute values
- **`tts_isnull[]`**: Boolean array for null flags
- **Lazy Loading**: Only fetch attributes when actually needed

## Performance Optimization Patterns

### 1. Projection Optimization
```c
// For SELECT id FROM table_with_100_columns:
// - PostgreSQL calls getsomeattrs(slot, 1)
// - Your function should ONLY fetch column 1
// - Result: 100x performance improvement vs fetching all 100
```

### 2. Caching Strategy
```c
// Use tts_nvalid to avoid re-fetching:
if (slot->tts_nvalid >= requested_attr) {
    return; // Already have this attribute
}
// Only fetch new attributes beyond tts_nvalid
```

### 3. Format-Specific Optimizations
- **Columnar**: Direct column access without row reconstruction
- **Compressed**: Decompress only needed attributes  
- **Optimized Row**: Use offset arrays for direct attribute access

## Common Mistakes in Table AM Development

### 1. **Eager Deformation** (Performance Killer)
```c
// WRONG: Always fetches all attributes
for (i = 1; i <= total_attributes; i++) { ... }

// RIGHT: Only fetch up to requested attribute
for (i = slot->tts_nvalid + 1; i <= natts; i++) { ... }
```

### 2. **Ignoring tts_nvalid State**
```c
// WRONG: Doesn't track what's already fetched
void bad_getsomeattrs(slot, natts) {
    fetch_all_attributes_up_to(natts);
}

// RIGHT: Tracks and builds on previous state
void good_getsomeattrs(slot, natts) {
    if (slot->tts_nvalid < natts) {
        fetch_attributes_from(slot->tts_nvalid + 1, natts);
    }
    slot->tts_nvalid = natts;
}
```

### 3. **Not Understanding Projection**
- **Wrong**: "natts parameter is total attributes to fetch"
- **Right**: "natts parameter is HIGHEST attribute number needed"

## Debugging TupleTableSlotOps Issues

### Performance Problems
- **Symptom**: Queries much slower than heap
- **Cause**: Usually incorrect `getsomeattrs()` implementation
- **Fix**: Implement proper lazy loading with `tts_nvalid` tracking

### Data Corruption
- **Symptom**: Wrong values returned from SELECT
- **Cause**: Incorrect attribute extraction in custom format
- **Fix**: Ensure insertion and extraction use identical offset calculations

## Best Practices

1. **Always implement lazy loading** in `getsomeattrs()`
2. **Track state** using `tts_nvalid` properly
3. **Test projection** with single-column SELECT on wide tables
4. **Verify correctness** before optimizing performance
5. **Handle system attributes** if your AM supports them

## Conclusion

`TupleTableSlotOps` is the **heart of Table AM performance**. A correct implementation of `getsomeattrs()` with proper projection optimization is essential for achieving performance competitive with or better than the standard heap access method.

The most common cause of Table AM performance problems is incorrect `getsomeattrs()` implementation that doesn't properly implement lazy attribute loading.
