# Optimized Row Format Performance Testing

This directory contains comprehensive testing tools to validate the performance improvements of the optimized row format compared to PostgreSQL's standard heap format.

## Test Files

### 1. `benchmark.sql` - Comprehensive Benchmark Suite
A full-featured benchmark that tests:
- **INSERT Performance** - Mixed data types (10,000 rows)
- **SELECT Performance** - Fixed-length and variable-length columns
- **NULL Handling** - Various null patterns and checking performance
- **Storage Efficiency** - Space usage comparison
- **Fixed-Length Column Performance** - Tables with mostly fixed-length columns
- **Variable-Length Column Performance** - Tables with mostly variable-length columns
- **Full Table Scan Performance** - Complete table traversal

### 2. `quick_test.sql` - Simple Validation Test
A lightweight test for basic validation:
- Simple table creation and data insertion
- Basic SELECT operations
- Storage size comparison
- Takes ~30 seconds to complete

### 3. `many_columns_test.sql` - Many Columns Performance Test
A focused test for tables with many columns:
- Creates tables with 100 columns (50 fixed + 50 variable)
- No primary keys (as per current limitation)
- Tests single-column selection performance
- Compares INSERT performance with 10,000 rows
- Tests storage efficiency
- Takes ~5-10 minutes to complete

### 4. `run_benchmark.sh` - Automated Test Runner
A shell script that automates the entire testing process:
- Checks PostgreSQL availability
- Creates test database
- Installs extension (optional)
- Runs benchmarks
- Analyzes and summarizes results
- Provides cleanup options

### 5. `run_many_columns_test.sh` - Many Columns Test Runner
A focused test runner for the many columns scenario:
- Specifically designed for tables with 100+ columns
- Tests single-column access patterns
- Compares fixed vs variable-length column performance
- Provides detailed performance analysis

## Quick Start

### Prerequisites
1. PostgreSQL must be running
2. Extension must be built and installed
3. User must have database creation privileges

### Run Quick Test (Recommended for first-time testing)
```bash
cd test
psql -d your_database -f quick_test.sql
```

### Run Full Benchmark
```bash
cd test
./run_benchmark.sh
```

### Run Full Benchmark with Extension Installation
```bash
cd test
./run_benchmark.sh --install
```

### Run Full Benchmark with Custom PostgreSQL
```bash
cd test
./run_benchmark.sh /path/to/postgres/install
```

### Run Many Columns Test (Recommended for Performance Validation)
```bash
cd test
./run_many_columns_test.sh
```

### Run Many Columns Test with Custom PostgreSQL
```bash
cd test
./run_many_columns_test.sh /path/to/postgres/install
```

### Verify Extension Installation
```bash
cd test
psql -d your_database -f verify_extension.sql
```

## Extension Management

### Automatic Extension Creation
All test scripts automatically create the extension if it doesn't exist:
```sql
CREATE EXTENSION IF NOT EXISTS optimized_row_format;
```

### Manual Extension Installation
If you need to install the extension manually:
```bash
# Build the extension
make clean && make

# Install to PostgreSQL
sudo make install

# Create extension in database
psql -d your_database -c "CREATE EXTENSION optimized_row_format;"
```

## Expected Performance Improvements

Based on the design, you should expect to see improvements in:

### 1. **INSERT Performance**
- **Expected**: 10-30% faster
- **Why**: Optimized memory layout and reduced overhead
- **Best case**: Tables with many fixed-length columns

### 2. **SELECT Performance (Fixed-length columns)**
- **Expected**: 20-50% faster
- **Why**: Contiguous storage of fixed-length data
- **Best case**: Queries accessing multiple fixed-length columns

### 3. **SELECT Performance (Variable-length columns)**
- **Expected**: 10-25% faster
- **Why**: Absolute offset access eliminates computation
- **Best case**: Tables with mixed column types

### 4. **NULL Handling**
- **Expected**: 15-40% faster
- **Why**: Conditional null bitmap (only when needed)
- **Best case**: Tables with few null values

### 5. **Storage Efficiency**
- **Expected**: 5-15% space savings
- **Why**: No null bitmap when no nulls exist
- **Best case**: Tables with no null values

## Interpreting Results

### Performance Metrics
- **Speedup**: Values > 1.0 indicate improvement
- **Time**: Lower is better
- **Storage**: Smaller is better

### Example Output
```
INSERT Performance (10,000 rows):
Heap format: 00:00:02.345
Optimized format: 00:00:01.789
Speedup: 1.31x

SELECT Performance (Fixed-length columns):
Heap format: 00:00:00.123
Optimized format: 00:00:00.089
Speedup: 1.38x
```

### What to Look For
1. **Consistent improvements** across multiple test runs
2. **Larger improvements** on fixed-length column access
3. **Storage savings** especially for tables without nulls
4. **Scalability** - improvements should increase with data size

## Troubleshooting

### Common Issues

#### 1. Extension Not Found
```
ERROR: access method "optimized_row_format" does not exist
```
**Solution**: The test scripts automatically create the extension. If this error persists:
```bash
# Build and install the extension
cd .. && make && sudo make install

# Then run the test again
cd test && ./run_benchmark.sh
```

#### 2. Permission Denied
```
ERROR: permission denied for database
```
**Solution**: Ensure you have database creation privileges or use an existing database

#### 3. PostgreSQL Not Running
```
ERROR: could not connect to server
```
**Solution**: Start PostgreSQL service

#### 4. Custom PostgreSQL Configuration Issues
```
ERROR: unrecognized configuration parameter "application_version"
```
**Solution**: Use the custom PostgreSQL directory option:
```bash
./run_benchmark.sh /path/to/your/postgres/install
```

#### 5. Extension Creation Fails
```
ERROR: could not open extension control file
```
**Solution**: Ensure the extension is properly installed:
```bash
# Check if extension files exist
ls -la /usr/local/pgsql/lib/postgresql/optimized_row_format*

# Reinstall if needed
cd .. && make clean && make && sudo make install
```

### Debug Mode
To see detailed extension logs, set the log level:
```sql
SET log_min_messages = debug1;
```

## Advanced Testing

### Custom Test Scenarios
You can modify `benchmark.sql` to test specific scenarios:

1. **Different data sizes**: Change the number of rows in INSERT loops
2. **Different column patterns**: Modify table schemas
3. **Different null patterns**: Adjust null generation logic
4. **Different query patterns**: Add custom SELECT statements

### Memory Profiling
For detailed memory analysis, use PostgreSQL's built-in functions:
```sql
-- Check buffer cache hit ratios
SELECT * FROM pg_stat_bgwriter;

-- Check table statistics
SELECT * FROM pg_stat_user_tables WHERE tablename LIKE 'test_%';
```

### System-Level Monitoring
Monitor system resources during tests:
```bash
# Monitor CPU and memory
top -p $(pgrep postgres)

# Monitor disk I/O
iostat -x 1
```

## Contributing Tests

When adding new tests:

1. **Follow the naming convention**: `test_heap_*` and `test_optimized_*`
2. **Include timing measurements**: Use `clock_timestamp()`
3. **Provide clear metrics**: Speedup ratios and absolute times
4. **Test edge cases**: Null values, empty strings, large objects
5. **Document expected results**: What improvements to expect

## Performance Baselines

For reference, here are typical baseline performance numbers on modern hardware:

- **INSERT (10K rows)**: 1-3 seconds
- **SELECT (fixed-length)**: 50-200ms
- **SELECT (variable-length)**: 100-500ms
- **NULL checking**: 20-100ms
- **Storage overhead**: 5-15% of data size

Your results may vary based on:
- Hardware specifications
- PostgreSQL configuration
- Data characteristics
- System load