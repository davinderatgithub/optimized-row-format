# PostgreSQL Table Access Method Implementation Patterns

**SME Documentation**: sme_01_postgres_expert  
**Date**: 2025-08-09  
**Purpose**: Reference guide for implementing compliant PostgreSQL Table Access Methods

## Overview

PostgreSQL's Table Access Method (AM) framework allows custom storage formats through the `TableAmRoutine` callback structure. This document outlines the essential patterns and requirements for compliant implementations.

## Core Architecture Requirements

### 1. Handler Function Pattern

Every Table AM must provide a handler function with this exact signature:
```c
Datum my_tableam_handler(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(my_tableam_handler);

Datum
my_tableam_handler(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(&my_tableam_methods);
}
```

### 2. TableAmRoutine Structure

The `TableAmRoutine` structure defines all callback functions. **Critical callbacks include**:

#### Scan Operations
- `slot_callbacks` - Return appropriate TupleTableSlotOps
- `scan_begin` - Initialize table scan
- `scan_getnextslot` - **CRITICAL**: Fetch next tuple during scan
- `scan_end` - Cleanup scan resources

#### DML Operations  
- `tuple_insert` - Insert new tuple
- `tuple_delete` - **REQUIRED**: Delete existing tuple
- `tuple_update` - **REQUIRED**: Update existing tuple  
- `tuple_lock` - Lock tuple for modification

#### Index Support
- `index_fetch_begin` - **REQUIRED for indexes**: Initialize index fetch
- `index_fetch_tuple` - **CRITICAL**: Convert ItemPointer to tuple
- `index_fetch_end` - Cleanup index fetch

### 3. MVCC Compliance

Table AMs **MUST** implement proper MVCC (Multi-Version Concurrency Control):
- Respect transaction visibility via `HeapTupleSatisfiesVisibility()`
- Properly set `t_xmin`, `t_xmax`, `t_cmin`, `t_cmax` in tuple headers
- Handle tuple versioning through `t_ctid` chaining

## Critical Implementation Patterns

### Scan Performance Pattern

**WRONG** (causes 1000x performance regression):
```c
static void 
my_getsomeattrs(TupleTableSlot *slot, int natts)
{
    // BAD: This deforms ALL attributes, not just requested ones
    for (i = 1; i <= natts; i++) {
        slot->tts_values[i - 1] = my_getattr(slot, i, &slot->tts_isnull[i - 1]);
    }
}
```

**CORRECT** (projection optimization):
```c
static void 
my_getsomeattrs(TupleTableSlot *slot, int natts)
{
    // GOOD: Only deform attributes not already deformed
    for (i = slot->tts_nvalid + 1; i <= natts; i++) {
        slot->tts_values[i - 1] = my_getattr(slot, i, &slot->tts_isnull[i - 1]);
    }
    slot->tts_nvalid = natts;
}
```

### Memory Layout Consistency Pattern

**Critical Rule**: Data insertion and extraction must use **identical** offset calculations.

**Example Issue**: 
- Insert: Store variable offset as `base_offset + fixed_len + var_pos`
- Extract: Read offset as `header + stored_offset` ❌ **WRONG**
- Extract: Read offset as `fixed_data_start + stored_offset` ✅ **CORRECT**

### DML Operation Pattern

**Never delegate DML to heap AM**:
```c
// WRONG:
static void my_tuple_delete(...) 
{
    const TableAmRoutine *heap_am = get_heap_am_routine();
    return heap_am->tuple_delete(...);  // Will corrupt data!
}

// CORRECT:
static void my_tuple_delete(...)
{
    // Implement MVCC-compliant deletion for custom format
    // Set t_xmax to current transaction ID
    // Follow visibility rules
}
```

## Reference Implementation Analysis

The `heapam_handler.c` provides the canonical reference implementation. Key patterns:

1. **Slot Operations**: Uses `TTSOpsBufferHeapTuple` for heap tuples
2. **Index Support**: Implements all `index_fetch_*` callbacks  
3. **MVCC**: Proper transaction ID handling throughout
4. **Error Handling**: Comprehensive validation and error reporting

## Common Anti-Patterns to Avoid

1. **Delegating to heap AM**: Custom formats require custom implementations
2. **Ignoring alignment**: Use `MAXALIGN()` for all data structures
3. **Incomplete MVCC**: Must handle all transaction states properly
4. **Performance regressions**: Implement projection optimization correctly
5. **Data corruption**: Ensure insertion/extraction offset calculations match

## Testing Requirements

Every Table AM implementation must pass:
1. **Correctness tests**: Data integrity across INSERT/SELECT cycles
2. **Performance tests**: No regressions vs heap AM for equivalent operations  
3. **DML tests**: UPDATE/DELETE operations work correctly
4. **Index tests**: Primary key and index creation/usage
5. **MVCC tests**: Proper transaction isolation and visibility

## Conclusion

Table Access Method development requires deep understanding of PostgreSQL internals, particularly MVCC, memory management, and the executor framework. The framework provides flexibility but demands strict adherence to patterns for data integrity and performance.
