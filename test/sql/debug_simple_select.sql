-- Simple SELECT Path Debugging
-- Minimal queries to trace the exact execution path differences

\echo '=== Simple SELECT Path Debug ==='

-- Create minimal test case
DROP TABLE IF EXISTS simple_heap, simple_optimized;

CREATE TABLE simple_heap (col1 INTEGER, col2 TEXT);
CREATE TABLE simple_optimized (col1 INTEGER, col2 TEXT) USING optimized_row_format;

INSERT INTO simple_heap VALUES (1, 'test');
INSERT INTO simple_optimized VALUES (1, 'test');

\echo '=== Query 1: First column access ==='
\echo '--- HEAP ---'
SELECT col1 FROM simple_heap;

\echo '--- OPTIMIZED ---'
SELECT col1 FROM simple_optimized;

\echo '=== Query 2: Second column access ==='
\echo '--- HEAP ---'
SELECT col2 FROM simple_heap;

\echo '--- OPTIMIZED ---'
SELECT col2 FROM simple_optimized;

\echo '=== Query 3: Both columns ==='
\echo '--- HEAP ---'
SELECT col1, col2 FROM simple_heap;

\echo '--- OPTIMIZED ---'
SELECT col1, col2 FROM simple_optimized;

\echo '=== Query 4: With WHERE clause ==='
\echo '--- HEAP ---'
SELECT col2 FROM simple_heap WHERE col1 = 1;

\echo '--- OPTIMIZED ---'
SELECT col2 FROM simple_optimized WHERE col1 = 1;
