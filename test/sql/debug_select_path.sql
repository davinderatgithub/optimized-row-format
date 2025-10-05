-- Debug SELECT Path Comparison: Heap vs Optimized Format
-- Simple queries to trace execution paths and identify performance bottlenecks

\timing on

-- Enable detailed logging for debugging
SET log_min_messages = debug1;
SET log_statement = 'all';
SET log_duration = on;
SET log_min_duration_statement = 0;

\echo '=== Creating test tables for SELECT path debugging ==='

-- Clean up
DROP TABLE IF EXISTS debug_heap;
DROP TABLE IF EXISTS debug_optimized;

-- Create simple test tables (small for easy debugging)
CREATE TABLE debug_heap (
    id INTEGER,
    name TEXT,
    value INTEGER,
    description TEXT
);

CREATE TABLE debug_optimized (
    id INTEGER,
    name TEXT,
    value INTEGER,
    description TEXT
) USING optimized_row_format;

-- Insert minimal test data
INSERT INTO debug_heap VALUES 
    (1, 'first', 100, 'test data one'),
    (2, 'second', 200, 'test data two'),
    (3, 'third', 300, 'test data three');

INSERT INTO debug_optimized SELECT * FROM debug_heap;

\echo '=== Basic SELECT path debugging ==='

\echo '--- HEAP: Select first column ---'
EXPLAIN (ANALYZE, BUFFERS, VERBOSE) 
SELECT id FROM debug_heap WHERE id = 2;

\echo '--- OPTIMIZED: Select first column ---'
EXPLAIN (ANALYZE, BUFFERS, VERBOSE) 
SELECT value FROM debug_optimized WHERE id = 2;

\echo '--- HEAP: Select last column ---'
EXPLAIN (ANALYZE, BUFFERS, VERBOSE) 
SELECT description FROM debug_heap WHERE id = 2;

\echo '--- OPTIMIZED: Select last column ---'
EXPLAIN (ANALYZE, BUFFERS, VERBOSE) 
SELECT description FROM debug_optimized WHERE id = 2;

\echo '=== Single row SELECT with different column positions ==='

\echo '--- HEAP: All columns ---'
SELECT * FROM debug_heap WHERE id = 2;

\echo '--- OPTIMIZED: All columns ---'
SELECT * FROM debug_optimized WHERE id = 2;

\echo '--- HEAP: First column only ---'
SELECT id FROM debug_heap WHERE id = 2;

\echo '--- OPTIMIZED: First column only ---'
SELECT id FROM debug_optimized WHERE id = 2;

\echo '--- HEAP: Middle column only ---'
SELECT value FROM debug_heap WHERE id = 2;

\echo '--- OPTIMIZED: Middle column only ---'
SELECT value FROM debug_optimized WHERE id = 2;

\echo '--- HEAP: Last column only ---'
SELECT description FROM debug_heap WHERE id = 2;

\echo '--- OPTIMIZED: Last column only ---'
SELECT description FROM debug_optimized WHERE id = 2;

\echo '=== Function call tracing (enable in PostgreSQL logs) ==='

-- These queries will show function calls in the logs
\echo '--- Trace heap tuple access ---'
SELECT id, name FROM debug_heap LIMIT 1;

\echo '--- Trace optimized tuple access ---'
SELECT id, name FROM debug_optimized LIMIT 1;

\echo '=== Performance comparison with timing ==='

\echo '--- HEAP: Timed single column access ---'
DO $$
DECLARE
    start_time TIMESTAMP;
    end_time TIMESTAMP;
    result_id INTEGER;
BEGIN
    start_time := clock_timestamp();
    SELECT id INTO result_id FROM debug_heap WHERE id = 2;
    end_time := clock_timestamp();
    RAISE NOTICE 'HEAP single column: % microseconds, result: %', 
                 EXTRACT(MICROSECONDS FROM (end_time - start_time)), result_id;
END $$;

\echo '--- OPTIMIZED: Timed single column access ---'
DO $$
DECLARE
    start_time TIMESTAMP;
    end_time TIMESTAMP;
    result_id INTEGER;
BEGIN
    start_time := clock_timestamp();
    SELECT id INTO result_id FROM debug_optimized WHERE id = 2;
    end_time := clock_timestamp();
    RAISE NOTICE 'OPTIMIZED single column: % microseconds, result: %', 
                 EXTRACT(MICROSECONDS FROM (end_time - start_time)), result_id;
END $$;

\echo '=== Debugging complete ==='
\echo 'Check PostgreSQL logs for detailed function call traces'
\echo 'Look for calls to:'
\echo '  - heap_getnext, heap_getattr (heap format)'
\echo '  - optimized_getnext, optimized_extract_attribute (optimized format)'
\echo '  - tts_optimized_getsomeattrs, tts_optimized_materialize'
