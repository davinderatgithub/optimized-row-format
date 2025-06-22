# Optimized Row Format Extension for PostgreSQL

This extension provides an optimized row format for PostgreSQL tables, focusing on efficient data layout and access patterns.

## Overview

The optimized storage format reorganizes table data to improve performance by:
- Storing fixed-length columns at the front
- Storing variable-length columns at the back
- Using an array of offsets for variable-length columns
- Minimizing padding and alignment overhead

## Project Directory Structure

```
contrib/optimized_row_format/
├── optimized_row_format.c          # Main implementation file
├── optimized_row_format.h          # Header file with structure definitions
├── optimized_row_format.control    # Extension control file
├── Makefile                        # Build configuration
├── README.md                       # This documentation
├── .gitignore                      # Git ignore patterns
├── sql/                            # SQL scripts directory
│   └── optimized_row_format--1.0.sql  # Extension installation script
└── test/                           # Test suite directory
    ├── sql/                        # Test SQL files
    │   ├── optimized_row_format.sql   # Basic functionality tests
    │   └── correctness.sql            # Correctness comparison tests
    └── expected/                   # Expected test outputs
        ├── optimized_row_format.out   # Expected output for basic tests
        └── correctness.out            # Expected output for correctness tests
```

## Column Order and Physical Layout

### Why Column Order Matters
The order of columns in a table can significantly impact performance, especially for wide tables. Our implementation takes advantage of this by optimizing the physical layout while maintaining compatibility with PostgreSQL's logical column ordering.

### Column Order Mapping
We deliberately avoid storing an explicit mapping between user-defined column order and physical storage order for several reasons:

1. **Deterministic Mapping**: The physical layout is deterministic - fixed-length columns are stored first, followed by variable-length columns. This makes the mapping predictable and calculable.

2. **Existing Metadata**: The original column order is already stored in PostgreSQL's system catalog (`pg_attribute`), so we don't need to duplicate this information.

3. **Reduced Overhead**: Storing an additional mapping would increase metadata overhead and add complexity to the storage format.

4. **On-the-fly Calculation**: Physical positions can be calculated efficiently using column attributes and the deterministic layout rules.

### Physical Position Calculation
The extension provides utility functions to calculate physical positions and offsets:
- `get_physical_position`: Determines where a column is stored in the physical layout
- `get_column_offset`: Calculates the actual offset to a column's data

These functions handle both fixed and variable-length columns, taking into account:
- Column type (fixed vs variable length)
- Original column position
- Dropped columns
- Alignment requirements

## Features

- Optimized tuple header structure
- Efficient handling of fixed-length columns
- Improved variable-length column storage
- Enhanced scanning performance

## Current PostgreSQL Row Format

PostgreSQL handles variable-length columns differently from fixed-length columns, using a specialized format and storage strategy to optimize space and access efficiency.

### Storage Format

- Variable-length columns (such as `text`, `varchar`, and other types with `attlen = -1`) are stored inline within the tuple using a structure called **`varlena`**.
- The `varlena` format includes a 4-byte header that stores the total length of the data and some flag bits indicating compression or external storage status.
- The actual data follows this header and can vary in size depending on the stored value.
- There is **no offset array stored in the tuple header** for variable-length columns. Instead, PostgreSQL calculates the position of variable-length columns by scanning preceding columns sequentially, using their length information.

### TOAST (The Oversized-Attribute Storage Technique)

- If a variable-length value is very large, PostgreSQL may store it **out-of-line** in a separate TOAST table to avoid bloating the main table and to improve access speed for smaller columns.
- TOAST supports compression and out-of-line storage strategies:
  - `EXTENDED` (default): Allows both compression and out-of-line storage.
  - `EXTERNAL`: Allows out-of-line storage without compression, which can speed up substring operations.
- The tuple stores a pointer to the TOASTed value instead of the full data inline.

### Access Mechanism

- When accessing a variable-length column, PostgreSQL reads the tuple sequentially to locate the column's data, relying on the length headers of preceding variable-length columns.
- Fixed-length columns allow direct offset calculation, but variable-length columns require this sequential approach due to their varying size.
- The `varlena` header flags help PostgreSQL determine if the data is compressed, stored externally, or stored inline.

### Tuple (Row) Format Diagram and Tuple Header Definition

Each row (tuple) in PostgreSQL is stored as follows:

```

+-------------------+----------------+----------------+----------------+
| Tuple Header (23 bytes fixed + variable) | Null Bitmap (optional) | Column Data (fixed + variable length) |
+-------------------+----------------+----------------+----------------+

```

- **Tuple Header** (`HeapTupleHeaderData`):
  - **t_xmin**: Transaction ID that inserted the tuple.
  - **t_xmax**: Transaction ID that deleted or updated the tuple (0 if none).
  - **t_cid**: Command ID within the transaction.
  - **t_ctid**: Item pointer to the current or newer version of the tuple (for MVCC).
  - **t_infomask2**: Number of attributes and various flags.
  - **t_infomask**: Flags indicating tuple status (e.g., presence of nulls).
  - **t_hoff**: Offset to the start of the actual data (includes header and null bitmap size).
  - **t_bits**: Variable-length bitmap indicating which columns are NULL.

- **Null Bitmap**: Present only if the tuple has nullable columns; bitmap size depends on the number of columns.

- **Column Data**: Columns stored sequentially; fixed-length columns have known size and alignment, variable-length columns stored as `varlena` structures.

### Visual Representation

```

|----------------------------- PostgreSQL Tuple -----------------------------|


| Tuple Header (23 bytes +) | Null Bitmap (if any) | Column 1 | Column 2 | ... |
| :-- | :-- | :-- | :-- | :-- |

```

- The header size (`t_hoff`) includes the fixed header plus the null bitmap length.
- Data columns follow immediately after the header.
- Fixed-length columns can be accessed by direct offset calculation.
- Variable-length columns require scanning previous columns due to varying sizes.

---

### Summary Table

| Aspect                          | Description                                                                                   |
|--------------------------------|-----------------------------------------------------------------------------------------------|
| Storage format                 | `varlena` structure with 4-byte length header + data                                         |
| Offset array in tuple header?  | No                                                                                            |
| Access method                 | Sequential scan of preceding columns using length headers                                     |
| Large values                  | Stored externally in TOAST tables with pointers in the tuple                                  |
| Compression                  | Supported via TOAST with automatic compression of large values                                |
| Tuple header size             | Fixed 23 bytes + variable null bitmap                                                        |

---

## Proposed Changes

The optimized storage format introduces several improvements to the current PostgreSQL row format:

### Key Changes

1. **Reorganized Column Layout**
   - Fixed-length columns are stored at the front
   - Variable-length columns are stored at the back
   - Offset array for variable-length columns is stored after the null bitmap

2. **Direct Variable-Length Column Access**
   - Offset array eliminates the need to scan through preceding columns
   - Enables O(1) access to any variable-length column
   - Particularly beneficial for tables with many columns

### Proposed Tuple Format Diagram
|----------------------------- PostgreSQL Tuple -----------------------------|
| Tuple Header | Null Bitmap | Variable-Length Offset Array | Fixed-Length Columns | Variable-Length Columns |
|  (fixed)    |  (variable) |        (variable)            |      (fixed)         |        (variable)       |
|-------------|-------------|-----------------------------|----------------------|------------------------|

### Benefits

1. **Improved Access Performance**
   - Direct access to variable-length columns without sequential scanning
   - Reduced CPU overhead for column access
   - Better cache utilization

2. **Optimized Storage**
   - Fixed-length columns remain efficiently packed
   - Variable-length columns are grouped together
   - Offset array provides quick access to variable-length data

3. **Backward Compatibility**
   - Maintains compatibility with existing PostgreSQL features
   - Preserves TOAST functionality
   - Supports all data types

### References

- [PostgreSQL Official Documentation: Database Page Layout](https://www.postgresql.org/docs/current/storage-page-layout.html)
- [PostgreSQL Official Documentation: Storage File Layout](https://www.postgresql.org/docs/current/storage-file-layout.html)
- [PostgreSQL TOAST Storage](https://www.postgresql.org/docs/current/storage-toast.html)
- Fujitsu Enterprise Postgres article on [PostgreSQL Row Storage](https://www.postgresql.fastware.com/pzone/2025-01-postgresql-row-storage)
- PostgreSQL source code header: `HeapTupleHeaderData` structure ([see htup_details.h](https://github.com/postgres/postgres/blob/master/src/include/access/htup_details.h))

This layout and explanation clarify how PostgreSQL organizes rows on disk, how the tuple header metadata supports MVCC and null handling, and how variable-length columns are stored and accessed without an explicit offset array.


## Building

The extension is located in the PostgreSQL contrib directory. To build it:

```bash
cd contrib/optimized_row_format
make
make install
```

## Testing

### Running Tests

The extension includes a comprehensive test suite to verify functionality and correctness. Tests are implemented using PostgreSQL's regression testing framework.

#### Running All Tests

To run the complete test suite:

```bash
cd contrib/optimized_row_format
make installcheck
```

This will run both the basic functionality tests and correctness comparison tests.

#### Running Individual Tests

To run specific test files:

```bash
# Run basic functionality tests only
make installcheck REGRESS=optimized_row_format

# Run correctness comparison tests only
make installcheck REGRESS=correctness
```

#### Test Structure

The test suite is organized into two main categories:

1. **Basic Functionality Tests** (`test/sql/optimized_row_format.sql`)
   - Extension creation and loading
   - Table creation with optimized storage
   - Basic INSERT and SELECT operations
   - Data type compatibility verification

2. **Correctness Tests** (`test/sql/correctness.sql`)
   - Side-by-side comparison of heap vs optimized storage
   - Identical data inserted into both storage formats
   - Verification that query results are identical
   - Partial column selection tests

#### Test Requirements

- PostgreSQL server must be running and accessible
- Extension must be compiled and installed
- User must have privileges to create extensions and tables
- Tests run in a temporary database schema

#### Expected Test Output

Test results are compared against expected output files in `test/expected/`. If tests pass, you'll see:

```
============== running regression test queries        ==============
test optimized_row_format         ... ok
test correctness                  ... ok
============== All 2 tests passed. ==============
```

#### Test Failures

If tests fail, examine the differences between actual and expected output:

```bash
# View test differences
diff test/results/optimized_row_format.out test/expected/optimized_row_format.out
diff test/results/correctness.out test/expected/correctness.out
```

### Manual Testing

You can also test the extension manually:

```sql
-- Load the extension
CREATE EXTENSION optimized_row_format;

-- Create a test table
CREATE TABLE test_table (
    id integer,
    name text,
    value bigint,
    flag boolean
) USING optimized_row_format;

-- Insert test data
INSERT INTO test_table VALUES (1, 'test', 100, true);

-- Query the data
SELECT * FROM test_table;
```

### Physical Validation

You can use the `pageinspect` extension to physically validate the row format:

```sql
-- Install pageinspect if not already available
CREATE EXTENSION IF NOT EXISTS pageinspect;

-- Examine the page structure
SELECT * FROM heap_page_items(get_raw_page('test_table', 0));
```

## Usage

After installation, you can create a table using the optimized row format:

```sql
CREATE EXTENSION optimized_row_format;

CREATE TABLE my_table (
    id integer,
    name text,
    data bytea
) USING optimized_row_format;
```

## Implementation Details

The extension implements several optimizations:

1. **Tuple Header Optimization**
   - Reduced header size
   - Efficient null bitmap storage
   - Optimized alignment

2. **Fixed-Length Column Handling**
   - Direct value storage
   - No alignment overhead
   - Efficient access patterns

3. **Variable-Length Column Storage**
   - Optimized TOAST storage
   - Efficient length encoding
   - Improved compression



## Testing

- pageinspect can be used to phsycally validate the row format

## Todo
- Fetching only the column needed.


## Development

This extension is part of the PostgreSQL contrib modules. To contribute:

1. Fork the PostgreSQL repository
2. Make your changes in the contrib/optimized_storage directory
3. Submit a patch to the PostgreSQL mailing list

## License

This extension is part of PostgreSQL and is released under the PostgreSQL License.