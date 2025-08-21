-- Profiling test for single-column SELECT performance
-- This test isolates different components to identify bottlenecks

\echo '=== Single Column SELECT Performance Profiling ==='

-- Create extension if needed
CREATE EXTENSION IF NOT EXISTS optimized_row_format;

-- Clean up any existing tables
DROP TABLE IF EXISTS heap_profile;
DROP TABLE IF EXISTS optimized_profile;

-- Create simple test tables with different column types
CREATE TABLE heap_profile (
    id INTEGER,
    fixed_col INTEGER,
    var_col TEXT
);

CREATE TABLE optimized_profile (
    id INTEGER,
    fixed_col INTEGER,
    var_col TEXT
) USING optimized_row_format;

-- Insert a reasonable amount of test data
INSERT INTO heap_profile 
SELECT i, i*10, 'text_data_' || i 
FROM generate_series(1, 10000) i;

INSERT INTO optimized_profile 
SELECT i, i*10, 'text_data_' || i 
FROM generate_series(1, 10000) i;

\timing on

\echo '--- Test 1: Fixed-length column (INTEGER) ---'
\echo 'Heap:'
SELECT COUNT(*) FROM heap_profile WHERE fixed_col > 50000;

\echo 'Optimized:'
SELECT COUNT(*) FROM optimized_profile WHERE fixed_col > 50000;

\echo '--- Test 2: Variable-length column (TEXT) ---'  
\echo 'Heap:'
SELECT COUNT(*) FROM heap_profile WHERE var_col LIKE '%500%';

\echo 'Optimized:'
SELECT COUNT(*) FROM optimized_profile WHERE var_col LIKE '%500%';

\echo '--- Test 3: Simple projection (single column) ---'
\echo 'Heap:'
SELECT fixed_col FROM heap_profile LIMIT 1000;

\echo 'Optimized:'
SELECT fixed_col FROM optimized_profile LIMIT 1000;

\echo '--- Test 4: Variable column projection ---'
\echo 'Heap:'
SELECT var_col FROM heap_profile LIMIT 1000;

\echo 'Optimized:'
SELECT var_col FROM optimized_profile LIMIT 1000;

\echo '--- Test 5: Multiple runs for consistency ---'
\echo 'Running each test 3 times for average...'

\echo 'Fixed column (heap):'
SELECT COUNT(*) FROM heap_profile WHERE fixed_col > 50000;
SELECT COUNT(*) FROM heap_profile WHERE fixed_col > 50000;
SELECT COUNT(*) FROM heap_profile WHERE fixed_col > 50000;

\echo 'Fixed column (optimized):'
SELECT COUNT(*) FROM optimized_profile WHERE fixed_col > 50000;
SELECT COUNT(*) FROM optimized_profile WHERE fixed_col > 50000;
SELECT COUNT(*) FROM optimized_profile WHERE fixed_col > 50000;

\echo '=== Profiling Complete ==='
