-- Test script for basic UPDATE operation in optimized row format
-- This script tests the basic functionality of the optimized_tuple_update function

\echo '=== UPDATE Operation Test Script for Optimized Row Format ==='

-- Clean up any existing test table
DROP TABLE IF EXISTS test_update_basic;

-- Create test table using optimized row format
CREATE TABLE test_update_basic (
    id int,
    name text,
    value int
) USING optimized_row_format;

\echo 'Test table created with optimized row format'

-- Insert test data
INSERT INTO test_update_basic VALUES 
    (1, 'Alice', 100),
    (2, 'Bob', 200),
    (3, 'Charlie', 300),
    (4, 'David', 400);

\echo 'Test data inserted'

-- Show initial data
\echo '--- Initial data ---'
SELECT * FROM test_update_basic ORDER BY id;

-- Test 1: UPDATE single row (simple case)
\echo '--- Test 1: UPDATE single row ---'
UPDATE test_update_basic SET value = 150 WHERE id = 1;

\echo 'Single row updated'
SELECT * FROM test_update_basic ORDER BY id;

-- Test 2: UPDATE multiple rows
\echo '--- Test 2: UPDATE multiple rows ---'
UPDATE test_update_basic SET value = value + 50 WHERE id IN (2, 3);

\echo 'Multiple rows updated'
SELECT * FROM test_update_basic ORDER BY id;

-- Test 3: UPDATE with text field change
\echo '--- Test 3: UPDATE text field ---'
UPDATE test_update_basic SET name = 'Updated_' || name WHERE id = 4;

\echo 'Text field updated'
SELECT * FROM test_update_basic ORDER BY id;

-- Test 4: UPDATE with no matches
\echo '--- Test 4: UPDATE with no matches ---'
UPDATE test_update_basic SET value = 999 WHERE id = 999;

\echo 'UPDATE with no matches completed'
SELECT * FROM test_update_basic ORDER BY id;

-- Test 5: UPDATE all rows
\echo '--- Test 5: UPDATE all rows ---'
UPDATE test_update_basic SET value = value * 2;

\echo 'All rows updated'
SELECT * FROM test_update_basic ORDER BY id;

-- Test 6: Transaction rollback test
\echo '--- Test 6: Transaction rollback test ---'
INSERT INTO test_update_basic VALUES (5, 'Eve', 500), (6, 'Frank', 600);

BEGIN;
UPDATE test_update_basic SET value = 777 WHERE id = 5;
SELECT * FROM test_update_basic WHERE id = 5;
ROLLBACK;

\echo 'Transaction rolled back'
SELECT * FROM test_update_basic ORDER BY id;

-- Test 7: Transaction commit test
\echo '--- Test 7: Transaction commit test ---'
BEGIN;
UPDATE test_update_basic SET name = 'Committed_Frank' WHERE id = 6;
SELECT * FROM test_update_basic WHERE id = 6;
COMMIT;

\echo 'Transaction committed'
SELECT * FROM test_update_basic ORDER BY id;

-- Test 8: Complex UPDATE with multiple columns
\echo '--- Test 8: Complex UPDATE with multiple columns ---'
UPDATE test_update_basic SET name = 'Multi_' || name, value = value + 100 WHERE id <= 3;

\echo 'Multiple columns updated'
SELECT * FROM test_update_basic ORDER BY id;

\echo '=== UPDATE Operation Test Complete ==='

-- Clean up
DROP TABLE test_update_basic;
