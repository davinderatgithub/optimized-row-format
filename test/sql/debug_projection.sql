-- Debug script to trace projection optimization execution
-- This will help identify why SELECT performance is so poor

\echo '=== Debugging Projection Optimization ==='

-- Create test extension and table
CREATE EXTENSION IF NOT EXISTS optimized_row_format;

-- Create a simple test table with mixed column types
CREATE TABLE projection_test (
    col1 int,
    col2 text,
    col3 int,
    col4 text,
    col5 int
) USING optimized_row_format;

\echo 'Table created successfully'

-- Insert a few test rows
INSERT INTO projection_test VALUES 
    (1, 'test1', 10, 'data1', 100),
    (2, 'test2', 20, 'data2', 200),
    (3, 'test3', 30, 'data3', 300);

\echo 'Test data inserted'

-- Enable logging to see what functions are being called
SET log_min_messages = NOTICE;

\echo 'Starting single-column SELECT (should be fast with projection)'
-- This should trigger optimized_getsomeattrs for only col1
SELECT col1 FROM projection_test LIMIT 1;

\echo 'Starting multi-column SELECT'
-- This should trigger optimized_getsomeattrs for col1, col2, col3
SELECT col1, col2, col3 FROM projection_test LIMIT 1;

\echo 'Starting full SELECT'
-- This should trigger optimized_getsomeattrs for all columns
SELECT * FROM projection_test LIMIT 1;

\echo 'Testing ORDER BY (may trigger materialization)'
SELECT col1, col2 FROM projection_test ORDER BY col1 LIMIT 1;

-- Reset logging
SET log_min_messages = WARNING;

-- Cleanup
DROP TABLE projection_test;

\echo 'Debug test completed'
