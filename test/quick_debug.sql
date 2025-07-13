-- Quick Debug Test for Optimized Row Format
-- This test creates a simple table and tries to insert/select data

\echo '=== Quick Debug Test ==='

-- Create the extension
CREATE EXTENSION IF NOT EXISTS optimized_row_format;

-- Check if extension is available
SELECT
    CASE
        WHEN EXISTS (
            SELECT 1 FROM pg_am WHERE amname = 'optimized_row_format'
        ) THEN '✅ Extension is available'
        ELSE '❌ Extension is NOT available'
    END as extension_status;

-- Create a simple test table
CREATE TABLE debug_test (
    id INTEGER,
    name TEXT
) USING optimized_row_format;

\echo '=== Testing INSERT ==='

-- Try to insert a single row
INSERT INTO debug_test (id, name) VALUES (1, 'test');

\echo '=== Testing SELECT ==='

-- Check if we can see the data
SELECT COUNT(*) as row_count FROM debug_test;
SELECT * FROM debug_test;

\echo '=== Testing table size ==='

-- Check table size
SELECT
    pg_size_pretty(pg_total_relation_size('debug_test')) as total_size,
    pg_size_pretty(pg_relation_size('debug_test')) as table_size;

\echo '=== Debug Complete ==='