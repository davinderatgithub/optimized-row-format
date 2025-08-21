-- Basic DELETE Operation Test
-- This test verifies the core functionality of the optimized_tuple_delete function

\echo '=== Basic DELETE Operation Test ==='

CREATE EXTENSION IF NOT EXISTS optimized_row_format;

-- Clean up any existing tables
DROP TABLE IF EXISTS test_delete_basic;

\echo '--- Test 1: Simple DELETE operation ---'

-- Create a simple test table
CREATE TABLE test_delete_basic (
    id INTEGER,
    name TEXT,
    value INTEGER
) USING optimized_row_format;

\echo 'Table created successfully'

-- Insert test data
INSERT INTO test_delete_basic VALUES 
    (1, 'Alice', 100),
    (2, 'Bob', 200),
    (3, 'Charlie', 300),
    (4, 'David', 400);

\echo 'Test data inserted'

-- Verify initial data
SELECT * FROM test_delete_basic ORDER BY id;

\echo '--- Test 2: DELETE single row ---'

-- Delete one record
DELETE FROM test_delete_basic WHERE id = 2;

\echo 'Single row deleted'

-- Verify deletion
SELECT * FROM test_delete_basic ORDER BY id;

\echo '--- Test 3: DELETE multiple rows ---'

-- Delete multiple records
DELETE FROM test_delete_basic WHERE value >= 300;

\echo 'Multiple rows deleted'

-- Verify deletion  
SELECT * FROM test_delete_basic ORDER BY id;

\echo '--- Test 4: DELETE with no matches ---'

-- Try to delete non-existent record
DELETE FROM test_delete_basic WHERE id = 999;

\echo 'DELETE with no matches completed'

-- Verify no change
SELECT * FROM test_delete_basic ORDER BY id;

\echo '--- Test 5: DELETE all remaining rows ---'

-- Delete all remaining records
DELETE FROM test_delete_basic;

\echo 'All remaining rows deleted'

-- Verify table is empty
SELECT COUNT(*) as remaining_count FROM test_delete_basic;

\echo '--- Test 6: Transaction rollback test ---'

-- Insert new data for transaction test
INSERT INTO test_delete_basic VALUES (5, 'Eve', 500), (6, 'Frank', 600);

BEGIN;
    DELETE FROM test_delete_basic WHERE id = 5;
    SELECT * FROM test_delete_basic ORDER BY id; -- Should show only Frank
ROLLBACK;

\echo 'Transaction rolled back'

-- Verify rollback worked
SELECT * FROM test_delete_basic ORDER BY id; -- Should show both Eve and Frank

\echo '--- Test 7: Concurrent visibility test ---'

-- Test MVCC behavior
BEGIN;
    DELETE FROM test_delete_basic WHERE id = 6;
    -- In this transaction, Frank should be gone
    SELECT * FROM test_delete_basic ORDER BY id;
COMMIT;

-- After commit, Frank should still be gone
SELECT * FROM test_delete_basic ORDER BY id;

\echo '=== DELETE Operation Test Complete ==='

-- Clean up
DROP TABLE test_delete_basic;
