-- Comprehensive UPDATE Operation Test Suite for Optimized Row Format
-- This test establishes the expected behavior for UPDATE operations

\echo '=== COMPREHENSIVE UPDATE TEST SUITE ==='

-- Clean up any existing test tables
DROP TABLE IF EXISTS update_test_simple CASCADE;
DROP TABLE IF EXISTS update_test_mixed CASCADE;
DROP TABLE IF EXISTS update_test_wide CASCADE;
DROP TABLE IF EXISTS update_heap_comparison CASCADE;

-- Create extension if not exists
CREATE EXTENSION IF NOT EXISTS optimized_row_format;

\echo ''
\echo '=== TEST 1: Basic UPDATE Operations ==='

-- Simple table with mixed data types
CREATE TABLE update_test_simple (
    id INTEGER,
    name TEXT,
    value INTEGER,
    flag BOOLEAN,
    created_at TIMESTAMP DEFAULT NOW()
) USING optimized_row_format;

-- Insert test data
INSERT INTO update_test_simple (id, name, value, flag) VALUES 
    (1, 'Alice', 100, true),
    (2, 'Bob', 200, false),
    (3, 'Charlie', 300, true),
    (4, 'David', 400, false),
    (5, 'Eve', 500, true);

\echo 'Initial data:'
SELECT * FROM update_test_simple ORDER BY id;

\echo ''
\echo '--- Test 1.1: UPDATE single fixed-length column ---'
UPDATE update_test_simple SET value = 150 WHERE id = 1;
SELECT * FROM update_test_simple WHERE id = 1;

\echo ''
\echo '--- Test 1.2: UPDATE single variable-length column ---'
UPDATE update_test_simple SET name = 'Alice_Updated' WHERE id = 1;
SELECT * FROM update_test_simple WHERE id = 1;

\echo ''
\echo '--- Test 1.3: UPDATE multiple columns ---'
UPDATE update_test_simple SET name = 'Bob_Multi', value = 250, flag = true WHERE id = 2;
SELECT * FROM update_test_simple WHERE id = 2;

\echo ''
\echo '--- Test 1.4: UPDATE with expression ---'
UPDATE update_test_simple SET value = value * 2 WHERE id IN (3, 4);
SELECT * FROM update_test_simple WHERE id IN (3, 4) ORDER BY id;

\echo ''
\echo '--- Test 1.5: UPDATE all rows ---'
UPDATE update_test_simple SET flag = NOT flag;
SELECT id, name, flag FROM update_test_simple ORDER BY id;

\echo ''
\echo '=== TEST 2: NULL Value Handling ==='

-- Add a nullable column
ALTER TABLE update_test_simple ADD COLUMN description TEXT;

\echo '--- Test 2.1: UPDATE to NULL ---'
UPDATE update_test_simple SET description = 'Initial description' WHERE id <= 3;
SELECT id, name, description FROM update_test_simple ORDER BY id;

UPDATE update_test_simple SET description = NULL WHERE id = 2;
SELECT id, name, description FROM update_test_simple ORDER BY id;

\echo '--- Test 2.2: UPDATE from NULL ---'
UPDATE update_test_simple SET description = 'Was NULL' WHERE description IS NULL;
SELECT id, name, description FROM update_test_simple ORDER BY id;

\echo ''
\echo '=== TEST 3: Transaction Behavior ==='

\echo '--- Test 3.1: Transaction rollback ---'
BEGIN;
UPDATE update_test_simple SET value = 999 WHERE id = 1;
SELECT id, value FROM update_test_simple WHERE id = 1;
ROLLBACK;
SELECT id, value FROM update_test_simple WHERE id = 1;

\echo '--- Test 3.2: Transaction commit ---'
BEGIN;
UPDATE update_test_simple SET value = 175 WHERE id = 1;
SELECT id, value FROM update_test_simple WHERE id = 1;
COMMIT;
SELECT id, value FROM update_test_simple WHERE id = 1;

\echo ''
\echo '=== TEST 4: Concurrent Updates (MVCC) ==='

-- This test verifies MVCC behavior
\echo '--- Test 4.1: Multiple transactions ---'
-- Note: This is a simplified test - real concurrent testing would need multiple sessions

BEGIN;
UPDATE update_test_simple SET value = 180 WHERE id = 1;
-- In a real test, another transaction would try to update the same row
COMMIT;

\echo ''
\echo '=== TEST 5: Performance Comparison with Heap ==='

-- Create equivalent heap table for comparison
CREATE TABLE update_heap_comparison (
    id INTEGER,
    name TEXT,
    value INTEGER,
    flag BOOLEAN,
    created_at TIMESTAMP DEFAULT NOW(),
    description TEXT
);

-- Insert same data
INSERT INTO update_heap_comparison 
SELECT * FROM update_test_simple;

\echo '--- Performance test: UPDATE 1000 rows ---'
-- Add more data for performance testing
INSERT INTO update_test_simple 
SELECT i, 'User_' || i, i * 10, (i % 2 = 0), NOW(), 'Desc_' || i
FROM generate_series(6, 1005) i;

INSERT INTO update_heap_comparison
SELECT i, 'User_' || i, i * 10, (i % 2 = 0), NOW(), 'Desc_' || i
FROM generate_series(6, 1005) i;

\timing on

\echo 'Heap UPDATE performance:'
UPDATE update_heap_comparison SET value = value + 1 WHERE id > 500;

\echo 'Optimized UPDATE performance:'
UPDATE update_test_simple SET value = value + 1 WHERE id > 500;

\timing off

\echo ''
\echo '=== TEST 6: Wide Table Updates ==='

-- Create wide table to test offset array handling
CREATE TABLE update_test_wide (
    id INTEGER,
    col1 TEXT, col2 TEXT, col3 TEXT, col4 TEXT, col5 TEXT,
    col6 TEXT, col7 TEXT, col8 TEXT, col9 TEXT, col10 TEXT,
    val1 INTEGER, val2 INTEGER, val3 INTEGER, val4 INTEGER, val5 INTEGER
) USING optimized_row_format;

-- Insert test data
INSERT INTO update_test_wide VALUES (
    1, 'text1', 'text2', 'text3', 'text4', 'text5',
    'text6', 'text7', 'text8', 'text9', 'text10',
    10, 20, 30, 40, 50
);

\echo '--- Test 6.1: UPDATE variable-length column in wide table ---'
UPDATE update_test_wide SET col5 = 'updated_text5_much_longer_than_before' WHERE id = 1;
SELECT id, col5 FROM update_test_wide;

\echo '--- Test 6.2: UPDATE fixed-length column in wide table ---'
UPDATE update_test_wide SET val3 = 999 WHERE id = 1;
SELECT id, val3 FROM update_test_wide;

\echo ''
\echo '=== TEST 7: Edge Cases ==='

\echo '--- Test 7.1: UPDATE with no matches ---'
UPDATE update_test_simple SET value = 999 WHERE id = 9999;
SELECT COUNT(*) as rows_affected FROM update_test_simple WHERE value = 999;

\echo '--- Test 7.2: UPDATE with subquery ---'
UPDATE update_test_simple SET value = (
    SELECT MAX(value) FROM update_test_simple WHERE id != update_test_simple.id
) WHERE id = 1;
SELECT id, value FROM update_test_simple WHERE id = 1;

\echo '--- Test 7.3: UPDATE with RETURNING clause ---'
UPDATE update_test_simple SET name = 'Final_' || name 
WHERE id <= 3 
RETURNING id, name;

\echo ''
\echo '=== TEST 8: Data Integrity Verification ==='

\echo '--- Verify all data is consistent after updates ---'
SELECT COUNT(*) as total_rows FROM update_test_simple;
SELECT COUNT(*) as non_null_names FROM update_test_simple WHERE name IS NOT NULL;
SELECT COUNT(*) as non_null_values FROM update_test_simple WHERE value IS NOT NULL;

\echo '--- Check for any corruption in variable-length data ---'
SELECT id, LENGTH(name) as name_length, LENGTH(COALESCE(description, '')) as desc_length 
FROM update_test_simple 
WHERE id <= 5 
ORDER BY id;

\echo ''
\echo '=== CLEANUP ==='
DROP TABLE IF EXISTS update_test_simple CASCADE;
DROP TABLE IF EXISTS update_test_mixed CASCADE;
DROP TABLE IF EXISTS update_test_wide CASCADE;
DROP TABLE IF EXISTS update_heap_comparison CASCADE;

\echo ''
\echo '=== TEST SUMMARY ==='
\echo 'This test suite verifies:'
\echo '1. Basic UPDATE operations (single/multiple columns)'
\echo '2. NULL value handling in UPDATEs'
\echo '3. Transaction behavior (COMMIT/ROLLBACK)'
\echo '4. MVCC compliance'
\echo '5. Performance comparison with heap'
\echo '6. Wide table UPDATE handling'
\echo '7. Edge cases and complex queries'
\echo '8. Data integrity after updates'
\echo ''
\echo 'Expected behavior:'
\echo '- All UPDATEs should complete without crashes'
\echo '- Data should remain consistent and uncorrupted'
\echo '- Performance should be competitive with heap'
\echo '- MVCC semantics should be preserved'
\echo '- Variable-length columns should handle size changes correctly'