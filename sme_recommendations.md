# SME Technical Recommendations: Optimized Row Format Fixes

**Author**: sme_01_postgres_expert  
**Date**: 2025-08-09  
**Work Item**: Personal-1-OptimizedRowFormat-FunctionalityTesting

## CRITICAL PRIORITY FIXES

### 1. Data Corruption Fix (BLOCKING ALL FUNCTIONALITY)

**Issue**: Mismatch between insertion and extraction offset calculations causing memory corruption.

**Root Cause**: Variable column offsets stored during insertion don't match how they're read during extraction.

**Immediate Fix Required**:

```c
// In optimized_extract_attribute() around line 555
// BEFORE (BROKEN):
uint32 absolute_offset = var_offsets[target_var_index];
char *var_data_ptr = (char *)header + absolute_offset;

// AFTER (FIXED):
// The stored offsets are relative to fixed_data start, not header
char *var_data_start = fixed_data + MAXALIGN(fixed_data_len);
uint32 var_offset = var_offsets[target_var_index];
char *var_data_ptr = var_data_start + var_offset;
```

**Testing**: After this fix, run `correctness.sql` test - all values should match between heap and optimized format.

### 2. Performance Bottleneck Fix (CRITICAL)

**Issue**: `optimized_getsomeattrs()` deforms ALL attributes instead of only requested ones.

**Current Code** (lines 619-622):
```c
for (i = 1; i <= natts; i++)  // WRONG: deforms ALL attributes
{
    slot->tts_values[i - 1] = optimized_getattr_for_slot(slot, i, &slot->tts_isnull[i - 1]);
}
```

**Fixed Code**:
```c
for (i = slot->tts_nvalid + 1; i <= natts; i++)  // CORRECT: only undeformed attributes
{
    slot->tts_values[i - 1] = optimized_getattr_for_slot(slot, i, &slot->tts_isnull[i - 1]);
}
```

**Testing**: After this fix, `SELECT id FROM table_with_many_columns` should be faster than heap, not slower.

## SECONDARY PRIORITY FIXES

### 3. Missing DML Operations

**Required Implementations**:

1. **optimized_tuple_delete()**:
```c
static void
optimized_tuple_delete(Relation rel, ItemPointer tid, CommandId cid,
                      Snapshot snapshot, Snapshot crosscheck, bool wait,
                      struct TM_Result *tmresult, struct TM_FailureData *tmfd)
{
    // Mark tuple as deleted by setting t_xmax to current transaction ID
    // Follow MVCC visibility rules
    // DO NOT delegate to heap AM
}
```

2. **optimized_tuple_update()**:
```c
static void
optimized_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot,
                      CommandId cid, Snapshot snapshot, Snapshot crosscheck,
                      bool wait, struct TM_Result *tmresult, struct TM_FailureData *tmfd)
{
    // 1. Mark old tuple as deleted (set t_xmax, update t_ctid)
    // 2. Insert new tuple using existing optimized_tuple_insert()
    // 3. Chain old and new tuples via ctid
}
```

### 4. Index Support

**Required Functions**:
```c
static IndexFetchTableData *
optimized_index_fetch_begin(Relation rel)
{
    // Initialize index fetch state
    // DO NOT delegate to heap AM
}

static bool
optimized_index_fetch_tuple(struct IndexFetchTableData *scan,
                            ItemPointer tid,
                            Snapshot snapshot,
                            TupleTableSlot *slot,
                            bool *call_again, bool *all_dead)
{
    // Convert ItemPointer to actual tuple
    // Use optimized format extraction
    // This is the critical function for index support
}
```

## IMPLEMENTATION SEQUENCE

1. **IMMEDIATE**: Fix data corruption in `optimized_extract_attribute()`
2. **IMMEDIATE**: Fix performance regression in `optimized_getsomeattrs()`
3. **Phase 2**: Implement DELETE operation
4. **Phase 3**: Implement UPDATE operation
5. **Phase 4**: Add index support functions
6. **Phase 5**: NULL value handling improvements

## TESTING VALIDATION

After each fix, verify:
1. **Data Corruption**: `correctness.sql` shows identical output for heap vs optimized
2. **Performance**: Single-column SELECT faster than heap on wide tables
3. **DML Operations**: UPDATE/DELETE work without errors
4. **Index Support**: Primary key creation succeeds

## POSTGRESQL AM COMPLIANCE

The extension must implement ALL required TableAmRoutine functions properly:
- ❌ **scan_getnextslot**: Currently has performance regression  
- ❌ **tuple_delete**: Currently delegates to heap (will fail)
- ❌ **tuple_update**: Currently delegates to heap (will fail)
- ❌ **index_fetch_tuple**: Currently delegates to heap (blocks indexes)

Each function MUST understand the optimized tuple format and NOT delegate to heap AM.

## RISK MITIGATION

**Before any code changes**:
1. Create backup of current working code
2. Add comprehensive logging to debug any new issues
3. Test incrementally - fix one issue at a time
4. Validate each fix with appropriate test cases

**Data integrity is paramount** - ensure no further corruption is introduced during fixes.
