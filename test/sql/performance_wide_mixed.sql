-- Wide Mixed Column Performance Test
-- Tests performance with wide tables using mixed data types

\timing on

\echo '=== Creating optimized_row_format extension ==='
CREATE EXTENSION IF NOT EXISTS optimized_row_format;

\echo '=== Setting up wide mixed test tables (30 columns) ==='

-- Clean up
DROP TABLE IF EXISTS test_heap_wide_mixed;
DROP TABLE IF EXISTS test_optimized_wide_mixed;

-- Create 30-column table with mixed data types (alternating pattern)
-- Pattern: INTEGER, TEXT, INTEGER, INTEGER, TEXT (repeating every 5 columns)
CREATE TABLE test_heap_wide_mixed (
    col1 INTEGER,   col2 TEXT,      col3 INTEGER,   col4 INTEGER,   col5 TEXT,
    col6 INTEGER,   col7 TEXT,      col8 INTEGER,   col9 INTEGER,   col10 TEXT,
    col11 INTEGER,  col12 TEXT,     col13 INTEGER,  col14 INTEGER,  col15 TEXT,
    col16 INTEGER,  col17 TEXT,     col18 INTEGER,  col19 INTEGER,  col20 TEXT,
    col21 INTEGER,  col22 TEXT,     col23 INTEGER,  col24 INTEGER,  col25 TEXT,
    col26 INTEGER,  col27 TEXT,     col28 INTEGER,  col29 INTEGER,  col30 TEXT
);

CREATE TABLE test_optimized_wide_mixed (
    col1 INTEGER,   col2 TEXT,      col3 INTEGER,   col4 INTEGER,   col5 TEXT,
    col6 INTEGER,   col7 TEXT,      col8 INTEGER,   col9 INTEGER,   col10 TEXT,
    col11 INTEGER,  col12 TEXT,     col13 INTEGER,  col14 INTEGER,  col15 TEXT,
    col16 INTEGER,  col17 TEXT,     col18 INTEGER,  col19 INTEGER,  col20 TEXT,
    col21 INTEGER,  col22 TEXT,     col23 INTEGER,  col24 INTEGER,  col25 TEXT,
    col26 INTEGER,  col27 TEXT,     col28 INTEGER,  col29 INTEGER,  col30 TEXT
) USING optimized_row_format;

\echo '=== Inserting test data (5,000 rows) ==='

-- Insert test data with mixed pattern
INSERT INTO test_heap_wide_mixed 
SELECT 
    -- Pattern: INTEGER, TEXT, INTEGER, INTEGER, TEXT (columns 1-5)
    i, 'text_' || i, i+1, i+2, 'data_' || i,
    -- Repeat pattern for columns 6-10
    i+3, 'text_' || (i+3), i+4, i+5, 'data_' || (i+3),
    -- Continue pattern for columns 11-15
    i+6, 'text_' || (i+6), i+7, i+8, 'data_' || (i+6),
    -- Pattern for columns 16-20
    i+9, 'text_' || (i+9), i+10, i+11, 'data_' || (i+9),
    -- Pattern for columns 21-25
    i+12, 'text_' || (i+12), i+13, i+14, 'data_' || (i+12),
    -- Final pattern for columns 26-30
    i+15, 'text_' || (i+15), i+16, i+17, 'LAST_COL_' || i
FROM generate_series(1, 5000) i;

INSERT INTO test_optimized_wide_mixed 
SELECT * FROM test_heap_wide_mixed;

\echo '=== PERFORMANCE TEST: SELECT Last Column (col30) ==='

DO $$
DECLARE
    heap_times NUMERIC[] := '{}';
    optimized_times NUMERIC[] := '{}';
    start_time TIMESTAMP;
    end_time TIMESTAMP;
    duration_ms NUMERIC;
    result_count INTEGER;
    i INTEGER;
    heap_avg NUMERIC;
    optimized_avg NUMERIC;
BEGIN
    RAISE NOTICE 'Testing LAST COLUMN (col30 TEXT) in 30-column mixed table - 5 iterations with warmup...';
    
    -- WARMUP
    SELECT COUNT(*) INTO result_count FROM test_heap_wide_mixed WHERE col30 LIKE 'LAST_COL_%';
    SELECT COUNT(*) INTO result_count FROM test_optimized_wide_mixed WHERE col30 LIKE 'LAST_COL_%';
    
    -- Test heap format (5 runs)
    FOR i IN 1..5 LOOP
        start_time := clock_timestamp();
        SELECT COUNT(*) INTO result_count FROM test_heap_wide_mixed WHERE col30 LIKE 'LAST_COL_%';
        end_time := clock_timestamp();
        duration_ms := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;
        heap_times := array_append(heap_times, duration_ms);
    END LOOP;
    
    -- Test optimized format (5 runs)
    FOR i IN 1..5 LOOP
        start_time := clock_timestamp();
        SELECT COUNT(*) INTO result_count FROM test_optimized_wide_mixed WHERE col30 LIKE 'LAST_COL_%';
        end_time := clock_timestamp();
        duration_ms := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;
        optimized_times := array_append(optimized_times, duration_ms);
    END LOOP;
    
    -- Calculate statistics
    SELECT AVG(x) INTO heap_avg FROM unnest(heap_times) AS x;
    SELECT AVG(x) INTO optimized_avg FROM unnest(optimized_times) AS x;
    
    RAISE NOTICE 'WIDE MIXED TABLE - Last Column (col30 TEXT) Performance:';
    RAISE NOTICE '  Heap - Avg: % ms', ROUND(heap_avg, 3);
    RAISE NOTICE '  Optimized - Avg: % ms', ROUND(optimized_avg, 3);
    RAISE NOTICE '  Speedup: %x', ROUND(heap_avg / NULLIF(optimized_avg, 0), 3);
    RAISE NOTICE '  Result count: %', result_count;
    RAISE NOTICE '  Pattern: INTEGER, TEXT, INTEGER, INTEGER, TEXT (repeating)';
END $$;

\echo '=== COMPARISON: First Column Performance ==='

DO $$
DECLARE
    heap_avg NUMERIC;
    optimized_avg NUMERIC;
    start_time TIMESTAMP;
    end_time TIMESTAMP;
    result_count INTEGER;
BEGIN
    -- Test first column for comparison
    start_time := clock_timestamp();
    SELECT COUNT(*) INTO result_count FROM test_heap_wide_mixed WHERE col1 > 2500;
    end_time := clock_timestamp();
    heap_avg := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;
    
    start_time := clock_timestamp();
    SELECT COUNT(*) INTO result_count FROM test_optimized_wide_mixed WHERE col1 > 2500;
    end_time := clock_timestamp();
    optimized_avg := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;
    
    RAISE NOTICE 'COMPARISON - First Column (col1 INTEGER) Performance:';
    RAISE NOTICE '  Heap: % ms, Optimized: % ms, Speedup: %x', 
                 ROUND(heap_avg, 3), ROUND(optimized_avg, 3), 
                 ROUND(heap_avg / NULLIF(optimized_avg, 0), 3);
    RAISE NOTICE '  Position impact: First vs Last column (1st vs 30th)';
END $$;

\echo '=== PERFORMANCE TEST: Middle Column (col15) ==='

DO $$
DECLARE
    heap_avg NUMERIC;
    optimized_avg NUMERIC;
    start_time TIMESTAMP;
    end_time TIMESTAMP;
    result_count INTEGER;
BEGIN
    -- Test middle column for comparison
    start_time := clock_timestamp();
    SELECT COUNT(*) INTO result_count FROM test_heap_wide_mixed WHERE col15 LIKE 'data_%';
    end_time := clock_timestamp();
    heap_avg := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;
    
    start_time := clock_timestamp();
    SELECT COUNT(*) INTO result_count FROM test_optimized_wide_mixed WHERE col15 LIKE 'data_%';
    end_time := clock_timestamp();
    optimized_avg := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;
    
    RAISE NOTICE 'MIDDLE COLUMN - col15 (TEXT) Performance:';
    RAISE NOTICE '  Heap: % ms, Optimized: % ms, Speedup: %x', 
                 ROUND(heap_avg, 3), ROUND(optimized_avg, 3), 
                 ROUND(heap_avg / NULLIF(optimized_avg, 0), 3);
    RAISE NOTICE '  Position: Middle column (15th of 30)';
END $$;

\echo '=== Storage Comparison ==='
SELECT 
    '30 Mixed Columns' as test_case,
    pg_size_pretty(pg_total_relation_size('test_heap_wide_mixed')) as heap_size,
    pg_size_pretty(pg_total_relation_size('test_optimized_wide_mixed')) as optimized_size,
    ROUND(
        (pg_total_relation_size('test_optimized_wide_mixed')::NUMERIC / 
         pg_total_relation_size('test_heap_wide_mixed')::NUMERIC - 1) * 100, 1
    ) || '%' as overhead_percent;

\echo '=== Wide Mixed Performance Test Complete ==='
\echo 'This test shows performance with mixed column types:'
\echo '- 30 columns with alternating INTEGER, TEXT pattern'
\echo '- Tests first, middle, and last column access'
\echo '- Shows how column position affects performance'
