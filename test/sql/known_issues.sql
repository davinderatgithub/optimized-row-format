-- Known Issues Test Suite for Optimized Row Format
-- This file contains tests that reproduce known bugs and limitations
-- These tests are expected to FAIL until the issues are resolved

\echo '=== KNOWN ISSUES TEST SUITE ==='
\echo 'These tests document known bugs and limitations'
\echo 'Expected result: FAILURES until issues are fixed'

-- Create the extension if it doesn't exist
CREATE EXTENSION IF NOT EXISTS optimized_row_format;

\echo ''
\echo '=== ISSUE 1: SERIAL/PRIMARY KEY + NULL Values Server Crash ==='
\echo 'Status: CRITICAL - Causes server crash when inserting NULL values'
\echo 'Root Cause: var_col_count mismatch between counting and storing phases'
\echo 'Technical Details: PRIMARY KEY triggers duplicate checking during INSERT'
\echo '  - INSERT with NULL values corrupts var_col_count in tuple'  
\echo '  - SELECT operations read corrupted var_col_count (e.g. 1936269427)'
\echo '  - Server crashes with: "server closed the connection unexpectedly"'
\echo 'Workaround: Use INTEGER columns instead of SERIAL/PRIMARY KEY'
\echo 'Fix Status: Core var_col_count counting bug FIXED in optimized_tuple_insert()'
\echo '  - Still need to implement proper SERIAL/PRIMARY KEY support'

-- This test will crash the server with NULL values in SERIAL tables
-- DO $$
-- BEGIN
--     BEGIN
--         DROP TABLE IF EXISTS test_optimized_mixed1;
--         CREATE TABLE test_optimized_mixed1 (
--             id SERIAL,
--             text_col TEXT,
--             varchar_col VARCHAR(100)
--         ) USING optimized_row_format;
--         
--         -- This INSERT will crash due to SERIAL constraint checking corrupted data
--         -- The NULL varchar_col triggers the var_col_count corruption bug
--         INSERT INTO test_optimized_mixed1 VALUES (
--             1,                                    -- id
--             'This is a test text column',        -- text_col
--             NULL                                  -- varchar_col (triggers crash)
--         );
--         
--         RAISE NOTICE 'UNEXPECTED: SERIAL+NULL test passed - issue may be fixed!';
--     EXCEPTION WHEN OTHERS THEN
--         RAISE NOTICE 'EXPECTED: SERIAL+NULL test failed with: %', SQLERRM;
--     END;
-- END $$;

\echo 'SERIAL+NULL test skipped to prevent server crash'
\echo 'To reproduce: uncomment the DO block above'

\echo ''
\echo '=== ISSUE 2: Performance Regressions ==='
\echo 'Status: MAJOR - Projection optimization not working properly'

-- Create test tables for performance comparison
DROP TABLE IF EXISTS perf_heap_wide;
DROP TABLE IF EXISTS perf_opt_wide;

CREATE TABLE perf_heap_wide (
    col1 INTEGER, col2 INTEGER, col3 INTEGER, col4 INTEGER, col5 INTEGER,
    col6 TEXT, col7 TEXT, col8 TEXT, col9 TEXT, col10 TEXT
);

CREATE TABLE perf_opt_wide (
    col1 INTEGER, col2 INTEGER, col3 INTEGER, col4 INTEGER, col5 INTEGER,
    col6 TEXT, col7 TEXT, col8 TEXT, col9 TEXT, col10 TEXT
) USING optimized_row_format;

-- Insert test data
INSERT INTO perf_heap_wide SELECT i, i*2, i*3, i*4, i*5, 
    'text'||i, 'text'||i, 'text'||i, 'text'||i, 'text'||i
FROM generate_series(1, 1000) i;

INSERT INTO perf_opt_wide SELECT i, i*2, i*3, i*4, i*5, 
    'text'||i, 'text'||i, 'text'||i, 'text'||i, 'text'||i
FROM generate_series(1, 1000) i;

\echo 'Testing single-column SELECT performance (should be faster with projection)...'

-- Time single column select (this should be much faster with projection)
\timing on
SELECT COUNT(*) FROM perf_heap_wide WHERE col1 > 500;
SELECT COUNT(*) FROM perf_opt_wide WHERE col1 > 500;
\timing off

\echo 'If optimized is slower than heap, projection optimization is not working'

\echo ''
\echo '=== ISSUE 3: PRIMARY KEY Creation Fails ==='
\echo 'Status: MAJOR - Index support not implemented'

DO $$
BEGIN
    BEGIN
        DROP TABLE IF EXISTS test_primary_key;
        CREATE TABLE test_primary_key (
            id INTEGER PRIMARY KEY,
            name TEXT
        ) USING optimized_row_format;
        
        RAISE NOTICE 'UNEXPECTED: PRIMARY KEY test passed - issue may be fixed!';
    EXCEPTION WHEN OTHERS THEN
        RAISE NOTICE 'EXPECTED: PRIMARY KEY test failed with: %', SQLERRM;
    END;
END $$;

\echo ''
\echo '=== ISSUE 4: UPDATE/DELETE Operations ==='
\echo 'Status: MAJOR - DML operations delegate to heap AM'

-- Create test table for DML operations
DROP TABLE IF EXISTS test_dml;
CREATE TABLE test_dml (
    id INTEGER,
    name TEXT,
    value INTEGER
) USING optimized_row_format;

INSERT INTO test_dml VALUES (1, 'original', 100);
INSERT INTO test_dml VALUES (2, 'original', 200);

\echo 'Testing UPDATE operation (may fail or corrupt data)...'
DO $$
BEGIN
    BEGIN
        UPDATE test_dml SET name = 'updated' WHERE id = 1;
        RAISE NOTICE 'UPDATE operation completed (check for data corruption)';
    EXCEPTION WHEN OTHERS THEN
        RAISE NOTICE 'UPDATE failed with: %', SQLERRM;
    END;
END $$;

\echo 'Testing DELETE operation (may fail or corrupt data)...'
DO $$
BEGIN
    BEGIN
        DELETE FROM test_dml WHERE id = 2;
        RAISE NOTICE 'DELETE operation completed (check for data corruption)';
    EXCEPTION WHEN OTHERS THEN
        RAISE NOTICE 'DELETE failed with: %', SQLERRM;
    END;
END $$;

-- Check if data is still consistent
SELECT 'After DML operations:' as status, * FROM test_dml ORDER BY id;

\echo ''
\echo '=== ISSUE 5: Storage Efficiency Regression ==='
\echo 'Status: MODERATE - Wide tables use more space than heap'

-- Check storage sizes from previous tests
SELECT 
    'Wide Table Storage' as test,
    pg_size_pretty(pg_total_relation_size('perf_heap_wide')) as heap_size,
    pg_size_pretty(pg_total_relation_size('perf_opt_wide')) as optimized_size,
    CASE 
        WHEN pg_total_relation_size('perf_opt_wide') > pg_total_relation_size('perf_heap_wide')
        THEN '❌ REGRESSION: Optimized uses more space'
        ELSE '✅ OK: Optimized uses less space'
    END as status;

\echo ''
\echo '=== CLEANUP ==='
DROP TABLE IF EXISTS perf_heap_wide;
DROP TABLE IF EXISTS perf_opt_wide;
DROP TABLE IF EXISTS test_dml;

\echo ''
\echo '=== KNOWN ISSUES SUMMARY ==='
\echo '1. SERIAL columns cause server crashes (CRITICAL)'
\echo '2. Performance regressions in projection optimization (MAJOR)'
\echo '3. PRIMARY KEY creation fails (MAJOR)'
\echo '4. UPDATE/DELETE operations may corrupt data (MAJOR)'
\echo '5. Storage efficiency regression for wide tables (MODERATE)'
\echo ''
\echo 'See recommended_next_steps.md for detailed action plans'
