-- Test to validate fast path optimization is working
-- This tests single vs multi-column access patterns

\echo '=== Fast Path Validation Test ==='

CREATE EXTENSION IF NOT EXISTS optimized_row_format;

DROP TABLE IF EXISTS test_fast_path;

CREATE TABLE test_fast_path (
    col1 INTEGER,
    col2 INTEGER,
    col3 TEXT
) USING optimized_row_format;

INSERT INTO test_fast_path 
SELECT i, i*2, 'text_' || i 
FROM generate_series(1, 5000) i;

\timing on

\echo '--- Test: Single column SELECT (should use fast path) ---'
SELECT col1 FROM test_fast_path LIMIT 1000;

\echo '--- Test: Multi-column SELECT (should use normal path) ---'
SELECT col1, col2 FROM test_fast_path LIMIT 1000;

\echo '--- Test: Single column WHERE (should use fast path per row) ---'
SELECT COUNT(*) FROM test_fast_path WHERE col1 > 2500;

\echo '--- Test: Multi-column WHERE (should use normal path) ---'
SELECT COUNT(*) FROM test_fast_path WHERE col1 > 2500 AND col2 < 7500;

\echo '=== Fast Path Test Complete ==='
