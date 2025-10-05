-- Set optimized_row_format as default table access method
-- This will make ALL new tables use optimized storage by default

\echo '=== Setting optimized_row_format as default table access method ==='

-- First ensure the extension is loaded
CREATE EXTENSION IF NOT EXISTS optimized_row_format;

-- Set as default for current session
SET default_table_access_method = 'optimized_row_format';

-- Verify the setting
SHOW default_table_access_method;

-- Test that new tables use optimized format by default
\echo '=== Testing default table creation ==='

DROP TABLE IF EXISTS test_default_format;

-- This should create a table using optimized_row_format
CREATE TABLE test_default_format (
    id INTEGER,
    name TEXT,
    value INTEGER
);

-- Verify the table access method
SELECT schemaname, tablename, tableowner, tablespace, hasindexes, hasrules, hastriggers 
FROM pg_tables 
WHERE tablename = 'test_default_format';

-- Check the actual access method
SELECT relname, relam, 
       (SELECT amname FROM pg_am WHERE oid = relam) as access_method
FROM pg_class 
WHERE relname = 'test_default_format';

-- Test basic operations
INSERT INTO test_default_format VALUES (1, 'test', 100);
SELECT * FROM test_default_format;

\echo '=== Default access method test complete ==='
