# Optimized Row Format Performance Testing

This directory contains comprehensive testing tools to validate the performance characteristics of the optimized row format compared to PostgreSQL's standard heap format.

> **How to read these numbers:** these benchmarks measure **CPU / cache** behavior, **not disk I/O**.
> ORF is a CPU-layer optimization — it does not reduce the number of pages read, and its larger tuples
> (from the offset array) can *increase* I/O. The SELECT/INSERT/storage results below should be
> interpreted in that light. See the **"What This Optimizes — and What It Does NOT"** section in the
> top-level [`README.md`](../README.md) before drawing conclusions.

## Working Test Files

### 1. `performance.sql` - Comprehensive Performance Suite ✅
The main performance test that covers:
- **INSERT Performance** - Mixed data types (10,000 rows)
- **SELECT Performance** - Fixed-length and variable-length columns  
- **NULL Handling** - Various null patterns and performance
- **Storage Efficiency** - Space usage comparison
- **Mixed Data Types** - Complex schemas with multiple column types
- **Memory Safety** - Validates no corruption or crashes

### 2. `performance_wide_mixed.sql` - Wide Table Test (30 Columns) ✅
Tests performance with moderately wide tables:
- **30 columns** with alternating INTEGER/TEXT pattern
- **5,000 rows** of test data
- Tests **first, middle, and last** column access performance
- **Position impact analysis** - shows how column position affects performance
- **Storage overhead measurement** for wide tables

### 3. `performance_extreme_width_final.sql` - Extreme Width Test (600 Columns) ✅
Tests performance at maximum practical table width:
- **600 columns** with alternating INTEGER/TEXT pattern (maximum that fits in PostgreSQL row size)
- **2,000 rows** with minimal data to fit row size limits
- Tests **first, middle (300th), and last (600th)** column access
- **Extreme position impact** - demonstrates scaling behavior
- **Storage overhead at scale** - shows format behavior with very wide tables
- **Critical for optimization insights** - reveals counterintuitive performance patterns
- Takes ~5-10 minutes to complete

## Key Performance Insights

### Performance Patterns Discovered:
1. **INSERT Performance**: ✅ **1.27x speedup** over heap (consistently good)
2. **SELECT Performance**: ❌ **1.4-2.4x slower** than heap (needs optimization)
3. **Scaling Behavior**: **Counterintuitive** - performance improves with more columns
4. **Position Impact**: **First column worst** (1.7-2.3x slower), **last column best** (1.4-2.0x slower)

### Critical Findings:
- **High fixed overhead** in attribute extraction dominates performance
- **Storage overhead expected** (58% for 30-col, 198% for 600-col)
- **All operations stable** after type safety fixes
- **No memory corruption** or crashes in current implementation

## Running Tests

```bash
# Run main performance suite
psql -d postgres -f sql/performance.sql

# Test wide table performance (30 columns)
psql -d postgres -f sql/performance_wide_mixed.sql

# Test extreme width (600 columns) - takes 5-10 minutes
psql -d postgres -f sql/performance_extreme_width_final.sql
```

## Test Status
- ✅ All tests pass without crashes
- ✅ INSERT operations work correctly
- ✅ SELECT operations work correctly  
- ❌ Performance regression needs optimization
- ⚠️ UPDATE operations have known issues (separate investigation needed)

## Additional Test Files

The `sql/` directory also contains various debugging and specialized tests:
- `correctness.sql` - Data integrity validation
- `smoke.sql` - Basic functionality test
- `debug_*.sql` - Various debugging utilities
- `test_*.sql` - Specific feature tests
- `update_*.sql` - UPDATE operation tests (known issues)

## Prerequisites
1. PostgreSQL must be running
2. Extension must be built and installed (`make && make install`)
3. User must have database creation privileges

## Performance Optimization Roadmap

Based on test results, the main optimization targets are:
1. **Reduce fixed overhead** in `tts_optimized_getsomeattrs()`
2. **Optimize first column access** (currently worst performing)
3. **Add fast paths** for common access patterns
4. **Improve tuple header parsing** efficiency

The counterintuitive scaling behavior (better with more columns) provides clear direction for where to focus optimization efforts.