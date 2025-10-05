-- Smart Extraction Smoke Test
-- Tests the bitmap registry and smart attribute extraction

\echo '=== Smart Extraction Test ==='

-- Create extension
CREATE EXTENSION IF NOT EXISTS optimized_row_format;

-- Create a wide table with optimized format
DROP TABLE IF EXISTS test_smart_extraction;
CREATE TABLE test_smart_extraction (
    col1 INTEGER,
    col2 TEXT,
    col3 INTEGER,
    col4 TEXT,
    col5 INTEGER,
    col6 TEXT,
    col7 INTEGER,
    col8 TEXT,
    col9 INTEGER,
    col10 TEXT,
    col11 INTEGER,
    col12 TEXT,
    col13 INTEGER,
    col14 TEXT,
    col15 INTEGER,
    col16 TEXT,
    col17 INTEGER,
    col18 TEXT,
    col19 INTEGER,
    col20 TEXT
) USING optimized_row_format;

-- Insert test data
INSERT INTO test_smart_extraction VALUES 
    (1, 'a', 2, 'b', 3, 'c', 4, 'd', 5, 'e', 6, 'f', 7, 'g', 8, 'h', 9, 'i', 10, 'j'),
    (11, 'k', 12, 'l', 13, 'm', 14, 'n', 15, 'o', 16, 'p', 17, 'q', 18, 'r', 19, 's', 20, 't'),
    (21, 'u', 22, 'v', 23, 'w', 24, 'x', 25, 'y', 26, 'z', 27, 'aa', 28, 'bb', 29, 'cc', 30, 'dd');

\echo '=== Test 1: Sparse column access (should use smart extraction) ==='
-- This should extract only col1, col5, and col10
SELECT col1, col5, col10 FROM test_smart_extraction WHERE col1 > 0;

\echo '=== Test 2: First and last columns ==='
-- This should extract only col1 and col20
SELECT col1, col20 FROM test_smart_extraction;

\echo '=== Test 3: Middle columns only ==='
-- This should extract only col8, col9, col10
SELECT col8, col9, col10 FROM test_smart_extraction WHERE col9 > 5;

\echo '=== Test 4: Single column projection ==='
-- This should extract only col15
SELECT col15 FROM test_smart_extraction;

\echo '=== Test 5: Expression with multiple columns ==='
-- This should extract col1, col5, col10
SELECT col1 + col5 AS sum1, col10 FROM test_smart_extraction WHERE col1 < 20;

\echo '=== Test 6: Whole-row reference (should use fallback) ==='
-- This should extract all columns
SELECT * FROM test_smart_extraction WHERE col1 = 1;

\echo '=== Test 7: COUNT with WHERE clause ==='
-- This should extract only col1
SELECT COUNT(*) FROM test_smart_extraction WHERE col1 > 10;

\echo '=== Test 8: Self-join ==='
-- This should extract col1 from both sides
SELECT t1.col1, t2.col1 
FROM test_smart_extraction t1 
JOIN test_smart_extraction t2 ON t1.col1 = t2.col1
WHERE t1.col1 < 15;

\echo '=== Test 9: Aggregate with GROUP BY ==='
-- This should extract col1 and col5
SELECT col1, SUM(col5) FROM test_smart_extraction GROUP BY col1;

\echo '=== Test 10: Complex WHERE clause ==='
-- This should extract col1, col5, col10, col15
SELECT col1, col15 
FROM test_smart_extraction 
WHERE col5 > 2 AND col10 = 'e';

\echo '=== All tests completed successfully! ==='

-- Cleanup
DROP TABLE test_smart_extraction;
