-- Contract Violation Test
-- This test specifically targets scenarios that would expose PostgreSQL contract violations

\echo '=== PostgreSQL Contract Violation Test ==='

-- Set optimized format as default
SET default_table_access_method = 'optimized_row_format';

-- Create test table
DROP TABLE IF EXISTS contract_test;
CREATE TABLE contract_test (
    col1 INTEGER,
    col2 TEXT,
    col3 INTEGER,
    col4 TEXT,
    col5 INTEGER,
    col6 TEXT,
    col7 INTEGER,
    col8 TEXT,
    col9 INTEGER,
    col10 TEXT
);

-- Insert test data
INSERT INTO contract_test VALUES 
    (1, 'a', 2, 'b', 3, 'c', 4, 'd', 5, 'e'),
    (10, 'x', 20, 'y', 30, 'z', 40, 'w', 50, 'v'),
    (100, 'p', 200, 'q', 300, 'r', 400, 's', 500, 't');

\echo '=== Test 1: Multi-column SELECT (triggers FETCHSOME) ==='
-- This should trigger EEOP_INNER_FETCHSOME followed by multiple EEOP_INNER_VAR
SELECT col1, col5, col10 FROM contract_test WHERE col1 > 0;

\echo '=== Test 2: Expression with early columns ==='
-- This forces access to early columns after FETCHSOME
SELECT col1 + col3, col8 FROM contract_test WHERE col2 = 'a';

\echo '=== Test 3: Complex WHERE clause ==='
-- Multiple column access in WHERE clause
SELECT col10 FROM contract_test WHERE col1 > 0 AND col3 < 100 AND col5 = 3;

\echo '=== Test 4: JOIN with multiple column access ==='
-- Self-join to test complex column access patterns
SELECT t1.col1, t1.col5, t2.col3, t2.col8 
FROM contract_test t1 
JOIN contract_test t2 ON t1.col1 = t2.col1 - 9;

\echo '=== Test 5: Aggregate with GROUP BY ==='
-- Aggregates often access multiple columns
SELECT col2, COUNT(*), SUM(col1 + col3 + col5) 
FROM contract_test 
GROUP BY col2;

\echo '=== Test 6: CASE expression ==='
-- CASE expressions can trigger complex column access
SELECT col1,
       CASE 
           WHEN col1 < 10 THEN col2
           WHEN col3 < 50 THEN col4  
           ELSE col6
       END as result
FROM contract_test;

\echo '=== Test 7: Subquery with column access ==='
-- Subqueries can expose contract violations
SELECT col1, col10 
FROM contract_test 
WHERE col3 IN (SELECT col5 FROM contract_test WHERE col7 > 0);

\echo '=== Test 8: ORDER BY multiple columns ==='
-- ORDER BY accesses multiple columns
SELECT col1, col5, col9 
FROM contract_test 
ORDER BY col2, col4, col6;

\echo '=== Contract violation test complete ==='
\echo 'If no crashes occurred, the contract violations may be less severe than expected.'
\echo 'However, this does not guarantee safety under all conditions.'
