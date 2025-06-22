-- Correctness test: Compare optimized format vs standard heap format

-- Create extension
CREATE EXTENSION IF NOT EXISTS optimized_row_format;

-- Create identical tables with different storage formats
CREATE TABLE test_heap (
    id int,
    name text,
    value bigint,
    flag boolean
) USING heap;

CREATE TABLE test_optimized (
    id int,
    name text,
    value bigint,
    flag boolean
) USING optimized_row_format;

-- Insert identical data into both tables (no NULL values)
INSERT INTO test_heap VALUES (1, 'first', 100, true);
INSERT INTO test_heap VALUES (2, 'second', 200, false);
INSERT INTO test_heap VALUES (3, 'third', 300, true);

INSERT INTO test_optimized VALUES (1, 'first', 100, true);
INSERT INTO test_optimized VALUES (2, 'second', 200, false);
INSERT INTO test_optimized VALUES (3, 'third', 300, true);

-- Compare outputs - they should be identical
SELECT 'HEAP' as format, * FROM test_heap ORDER BY id;
SELECT 'OPTIMIZED' as format, * FROM test_optimized ORDER BY id;

-- Test specific column selections
SELECT 'HEAP_PARTIAL' as format, id, name FROM test_heap WHERE id <= 3 ORDER BY id;
SELECT 'OPT_PARTIAL' as format, id, name FROM test_optimized WHERE id <= 3 ORDER BY id;

-- Cleanup
DROP TABLE test_heap;
DROP TABLE test_optimized;