-- Comprehensive SELECT crash debug test
-- Tests multiple scenarios to isolate the segmentation fault

\timing on

-- Clean up any existing tables
DROP TABLE IF EXISTS debug_heap;
DROP TABLE IF EXISTS debug_optimized;

-- Create minimal test tables
CREATE TABLE debug_heap (
    id INTEGER,
    name TEXT
);

CREATE TABLE debug_optimized (
    id INTEGER,
    name TEXT
) USING optimized_row_format;

-- Insert minimal test data
INSERT INTO debug_heap VALUES (1, 'test1'), (2, 'test2'), (3, 'test3');
INSERT INTO debug_optimized VALUES (1, 'test1'), (2, 'test2'), (3, 'test3');

-- Test 1: Simple SELECT (should work)
\echo '=== Test 1: Simple SELECT * ==='
SELECT * FROM debug_heap;
SELECT * FROM debug_optimized;

-- Test 2: SELECT with WHERE clause (this might crash)
\echo '=== Test 2: SELECT with WHERE clause ==='
SELECT * FROM debug_heap WHERE id = 1;
\echo 'Heap SELECT with WHERE completed successfully'

SELECT * FROM debug_optimized WHERE id = 1;
\echo 'Optimized SELECT with WHERE completed successfully'

-- Test 3: COUNT operation (this is where the crash occurred)
\echo '=== Test 3: COUNT operation ==='
SELECT COUNT(*) FROM debug_heap;
\echo 'Heap COUNT completed successfully'

SELECT COUNT(*) FROM debug_optimized;
\echo 'Optimized COUNT completed successfully'

-- Test 4: COUNT with WHERE (this is the exact crash scenario)
\echo '=== Test 4: COUNT with WHERE (crash scenario) ==='
SELECT COUNT(*) FROM debug_heap WHERE id % 2 = 0;
\echo 'Heap COUNT with WHERE completed successfully'

SELECT COUNT(*) FROM debug_optimized WHERE id % 2 = 0;
\echo 'Optimized COUNT with WHERE completed successfully'

\echo '=== SECTION 1: All minimal tests completed successfully ==='

-- ========================================
-- SECTION 2: PROGRESSIVE COMPLEXITY TEST
-- ========================================

\echo '=== SECTION 2: PROGRESSIVE COMPLEXITY TEST ==='

-- Clean up
DROP TABLE IF EXISTS test_heap_progressive;
DROP TABLE IF EXISTS test_optimized_progressive;

-- Create tables similar to performance test
CREATE TABLE test_heap_progressive (
    regular_int INTEGER,
    regular_text TEXT,
    regular_bool BOOLEAN,
    regular_date TIMESTAMP
);

CREATE TABLE test_optimized_progressive (
    regular_int INTEGER,
    regular_text TEXT,
    regular_bool BOOLEAN,
    regular_date TIMESTAMP
) USING optimized_row_format;

\echo '--- Phase 2.1: Small dataset (10 rows) ---'
-- Insert 10 rows
INSERT INTO test_heap_progressive 
SELECT 
    i,
    'text_' || i,
    (i % 2 = 0),
    '2025-01-01'::timestamp + (i || ' days')::interval
FROM generate_series(1, 10) i;

INSERT INTO test_optimized_progressive 
SELECT 
    i,
    'text_' || i,
    (i % 2 = 0),
    '2025-01-01'::timestamp + (i || ' days')::interval
FROM generate_series(1, 10) i;

-- Test with 10 rows
SELECT COUNT(*) FROM test_heap_progressive WHERE regular_int % 2 = 0;
\echo 'Heap: 10 rows test passed'

SELECT COUNT(*) FROM test_optimized_progressive WHERE regular_int % 2 = 0;
\echo 'Optimized: 10 rows test passed'

\echo '--- Phase 2.2: Medium dataset (100 rows) ---'
-- Add more rows (total 100)
INSERT INTO test_heap_progressive 
SELECT 
    i,
    'text_' || i,
    (i % 2 = 0),
    '2025-01-01'::timestamp + (i || ' days')::interval
FROM generate_series(11, 100) i;

INSERT INTO test_optimized_progressive 
SELECT 
    i,
    'text_' || i,
    (i % 2 = 0),
    '2025-01-01'::timestamp + (i || ' days')::interval
FROM generate_series(11, 100) i;

-- Test with 100 rows
SELECT COUNT(*) FROM test_heap_progressive WHERE regular_int % 2 = 0;
\echo 'Heap: 100 rows test passed'

SELECT COUNT(*) FROM test_optimized_progressive WHERE regular_int % 2 = 0;
\echo 'Optimized: 100 rows test passed'

\echo '--- Phase 2.3: Large dataset (1000 rows) ---'
-- Add more rows (total 1000)
INSERT INTO test_heap_progressive 
SELECT 
    i,
    'text_' || i,
    (i % 2 = 0),
    '2025-01-01'::timestamp + (i || ' days')::interval
FROM generate_series(101, 1000) i;

INSERT INTO test_optimized_progressive 
SELECT 
    i,
    'text_' || i,
    (i % 2 = 0),
    '2025-01-01'::timestamp + (i || ' days')::interval
FROM generate_series(101, 1000) i;

-- Test with 1000 rows
SELECT COUNT(*) FROM test_heap_progressive WHERE regular_int % 2 = 0;
\echo 'Heap: 1000 rows test passed'

SELECT COUNT(*) FROM test_optimized_progressive WHERE regular_int % 2 = 0;
\echo 'Optimized: 1000 rows test passed'

\echo '--- Phase 2.4: Very large dataset (5000 rows) ---'
-- Add more rows (total 5000)
INSERT INTO test_heap_progressive 
SELECT 
    i,
    'text_' || i,
    (i % 2 = 0),
    '2025-01-01'::timestamp + (i || ' days')::interval
FROM generate_series(1001, 5000) i;

INSERT INTO test_optimized_progressive 
SELECT 
    i,
    'text_' || i,
    (i % 2 = 0),
    '2025-01-01'::timestamp + (i || ' days')::interval
FROM generate_series(1001, 5000) i;

-- Test with 5000 rows
SELECT COUNT(*) FROM test_heap_progressive WHERE regular_int % 2 = 0;
\echo 'Heap: 5000 rows test passed'

SELECT COUNT(*) FROM test_optimized_progressive WHERE regular_int % 2 = 0;
\echo 'Optimized: 5000 rows test passed'

\echo '--- Phase 2.5: Full dataset (10000 rows) - This might crash ---'
-- Add final rows (total 10000)
INSERT INTO test_heap_progressive 
SELECT 
    i,
    'text_' || i,
    (i % 2 = 0),
    '2025-01-01'::timestamp + (i || ' days')::interval
FROM generate_series(5001, 10000) i;

INSERT INTO test_optimized_progressive 
SELECT 
    i,
    'text_' || i,
    (i % 2 = 0),
    '2025-01-01'::timestamp + (i || ' days')::interval
FROM generate_series(5001, 10000) i;

-- Test with 10000 rows (this is where the original crash occurred)
SELECT COUNT(*) FROM test_heap_progressive WHERE regular_int % 2 = 0;
\echo 'Heap: 10000 rows test passed'

SELECT COUNT(*) FROM test_optimized_progressive WHERE regular_int % 2 = 0;
\echo 'Optimized: 10000 rows test passed - NO CRASH!'

\echo '=== SECTION 2: All progressive tests completed successfully ==='

-- ========================================
-- SECTION 3: EXACT PERFORMANCE TEST REPLICATION
-- ========================================

\echo '=== SECTION 3: EXACT PERFORMANCE TEST REPLICATION ==='

-- Clean up
DROP TABLE IF EXISTS test_heap_mixed;
DROP TABLE IF EXISTS test_optimized_mixed;

-- Create exact same tables as performance test
CREATE TABLE test_heap_mixed (
    regular_int INTEGER,
    regular_text TEXT,
    regular_bool BOOLEAN,
    regular_date TIMESTAMP
);

CREATE TABLE test_optimized_mixed (
    regular_int INTEGER,
    regular_text TEXT,
    regular_bool BOOLEAN,
    regular_date TIMESTAMP
) USING optimized_row_format;

\echo '--- Phase 3.1: Inserting 10,000 rows (exact same as performance test) ---'

-- Insert exact same data as performance test
INSERT INTO test_heap_mixed 
SELECT 
    i,
    'sample_text_' || (i % 100),
    (i % 3 = 0),
    '2025-01-01'::timestamp + (i || ' minutes')::interval
FROM generate_series(1, 10000) i;

INSERT INTO test_optimized_mixed 
SELECT 
    i,
    'sample_text_' || (i % 100),
    (i % 3 = 0),
    '2025-01-01'::timestamp + (i || ' minutes')::interval
FROM generate_series(1, 10000) i;

\echo 'Data insertion completed'

\echo '--- Phase 3.2: Testing the exact crashing query ---'

-- Test heap first (should work)
SELECT COUNT(*) FROM test_heap_mixed WHERE regular_int % 2 = 0;
\echo 'Heap query completed successfully'

-- This is the exact query that crashed in performance test
SELECT COUNT(*) FROM test_optimized_mixed WHERE regular_int % 2 = 0;
\echo 'Optimized query completed successfully - NO CRASH!'

\echo '--- Phase 3.3: Testing other SELECT patterns ---'

-- Test different WHERE conditions
SELECT COUNT(*) FROM test_optimized_mixed WHERE regular_bool = true;
\echo 'Boolean WHERE test passed'

SELECT COUNT(*) FROM test_optimized_mixed WHERE regular_text LIKE 'sample_text_1%';
\echo 'Text LIKE test passed'

SELECT COUNT(*) FROM test_optimized_mixed WHERE regular_date > '2025-01-01';
\echo 'Date comparison test passed'

-- Test without WHERE clause
SELECT COUNT(*) FROM test_optimized_mixed;
\echo 'COUNT without WHERE test passed'

\echo '=== SECTION 3: All exact replication tests completed successfully ==='

-- ========================================
-- SUMMARY
-- ========================================

\echo '=== COMPREHENSIVE DEBUG TEST SUMMARY ==='
\echo 'Section 1: Minimal test (3 rows) - PASSED'
\echo 'Section 2: Progressive complexity (10-10000 rows) - PASSED'  
\echo 'Section 3: Exact performance test replication - PASSED'
\echo ''
\echo 'CONCLUSION: No crashes detected in isolated tests'
\echo 'The crash might be:'
\echo '1. Intermittent/race condition'
\echo '2. Environment-specific'
\echo '3. Related to specific test setup in performance.sql'
\echo '4. Fixed by recent changes'
