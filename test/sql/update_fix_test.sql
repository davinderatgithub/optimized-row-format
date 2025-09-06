-- Test to verify UPDATE operation fix
-- This test should pass if the critical UPDATE issues are resolved

\echo '=== UPDATE FIX VERIFICATION TEST ==='

-- Clean up
DROP TABLE IF EXISTS update_fix_test;

-- Create extension
CREATE EXTENSION IF NOT EXISTS optimized_row_format;

-- Create simple test table
CREATE TABLE update_fix_test (
    id INTEGER,
    name TEXT,
    value INTEGER
) USING optimized_row_format;

\echo 'Table created successfully'

-- Insert test data
INSERT INTO update_fix_test VALUES (1, 'Alice', 100), (2, 'Bob', 200);

\echo 'Initial data:'
SELECT * FROM update_fix_test ORDER BY id;

-- Test 1: UPDATE fixed-length column (this was causing crashes)
\echo 'Testing UPDATE of fixed-length column...'
UPDATE update_fix_test SET value = 150 WHERE id = 1;

\echo 'After fixed-length UPDATE:'
SELECT * FROM update_fix_test ORDER BY id;

-- Test 2: UPDATE variable-length column
\echo 'Testing UPDATE of variable-length column...'
UPDATE update_fix_test SET name = 'Alice_Updated' WHERE id = 1;

\echo 'After variable-length UPDATE:'
SELECT * FROM update_fix_test ORDER BY id;

-- Test 3: UPDATE multiple columns
\echo 'Testing UPDATE of multiple columns...'
UPDATE update_fix_test SET name = 'Bob_Multi', value = 250 WHERE id = 2;

\echo 'After multi-column UPDATE:'
SELECT * FROM update_fix_test ORDER BY id;

-- Test 4: Transaction rollback
\echo 'Testing transaction rollback...'
BEGIN;
UPDATE update_fix_test SET value = 999 WHERE id = 1;
SELECT value FROM update_fix_test WHERE id = 1;
ROLLBACK;
SELECT value FROM update_fix_test WHERE id = 1;

\echo 'UPDATE fix verification completed successfully!'

-- Clean up
DROP TABLE update_fix_test;
