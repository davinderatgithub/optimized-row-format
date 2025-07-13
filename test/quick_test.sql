-- Quick performance test for Optimized Row Format
-- This is a simplified test for basic validation

\timing on

-- Create the extension if it doesn't exist
CREATE EXTENSION IF NOT EXISTS optimized_row_format;

-- Verify extension is available
SELECT
    CASE
        WHEN EXISTS (
            SELECT 1 FROM pg_am WHERE amname = 'optimized_row_format'
        ) THEN '✅ Extension is available'
        ELSE '❌ Extension is NOT available - tests will fail'
    END as extension_status;

-- Create simple test tables
CREATE TABLE quick_heap (
    id INTEGER,
    name TEXT,
    value INTEGER,
    description TEXT
);

CREATE TABLE quick_optimized (
    id INTEGER,
    name TEXT,
    value INTEGER,
    description TEXT
) USING optimized_row_format;

-- Insert test data
INSERT INTO quick_heap (id, name, value, description)
SELECT
    i,
    'Name ' || i,
    i * 10,
    'Description for item ' || i || ' with some additional text to make it longer'
FROM generate_series(1, 1000) i;

INSERT INTO quick_optimized (id, name, value, description)
SELECT
    i,
    'Name ' || i,
    i * 10,
    'Description for item ' || i || ' with some additional text to make it longer'
FROM generate_series(1, 1000) i;

-- Test 1: Simple SELECT
SELECT 'Heap SELECT' as test, COUNT(*) as count FROM quick_heap WHERE value > 500;
SELECT 'Optimized SELECT' as test, COUNT(*) as count FROM quick_optimized WHERE value > 500;

-- Test 2: Text search
SELECT 'Heap TEXT' as test, COUNT(*) as count FROM quick_heap WHERE name LIKE '%50%';
SELECT 'Optimized TEXT' as test, COUNT(*) as count FROM quick_optimized WHERE name LIKE '%50%';

-- Test 3: Storage comparison
SELECT
    'Heap' as format,
    pg_size_pretty(pg_total_relation_size('quick_heap')) as size
UNION ALL
SELECT
    'Optimized' as format,
    pg_size_pretty(pg_total_relation_size('quick_optimized')) as size;

-- Cleanup
DROP TABLE quick_heap;
DROP TABLE quick_optimized;