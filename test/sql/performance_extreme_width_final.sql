-- Extreme Width Performance Test (Maximum Practical Width)
-- Tests performance with the maximum columns that fit within PostgreSQL row size limits

\timing on

\echo '=== Creating optimized_row_format extension ==='
CREATE EXTENSION IF NOT EXISTS optimized_row_format;

\echo '=== Finding Maximum Practical Column Count ==='

-- Clean up
DROP TABLE IF EXISTS test_heap_extreme_width;
DROP TABLE IF EXISTS test_optimized_extreme_width;

-- Create tables with maximum practical columns (using smaller data to fit in row size)
DO $$
DECLARE
    heap_sql TEXT := 'CREATE TABLE test_heap_extreme_width (';
    opt_sql TEXT := 'CREATE TABLE test_optimized_extreme_width (';
    col_def TEXT;
    i INTEGER;
    max_cols INTEGER := 600;  -- Conservative estimate
BEGIN
    -- Build column definitions dynamically
    FOR i IN 1..max_cols LOOP
        IF i > 1 THEN
            heap_sql := heap_sql || ', ';
            opt_sql := opt_sql || ', ';
        END IF;
        
        -- Alternating pattern: INTEGER, TEXT (smaller pattern to fit more columns)
        IF (i % 2 = 1) THEN
            col_def := 'col' || i || ' INTEGER';
        ELSE
            col_def := 'col' || i || ' TEXT';
        END IF;
        
        heap_sql := heap_sql || col_def;
        opt_sql := opt_sql || col_def;
    END LOOP;
    
    heap_sql := heap_sql || ')';
    opt_sql := opt_sql || ') USING optimized_row_format';
    
    -- Create both tables
    EXECUTE heap_sql;
    EXECUTE opt_sql;
    
    RAISE NOTICE 'Created %-column tables successfully', max_cols;
END $$;

\echo '=== Inserting test data (2,000 rows with minimal data) ==='

-- Insert test data using minimal data to fit within row size limits
DO $$
DECLARE
    insert_sql TEXT;
    i INTEGER;
    max_cols INTEGER := 600;
BEGIN
    -- Build INSERT statement using generate_series with minimal data
    insert_sql := 'INSERT INTO test_heap_extreme_width SELECT ';
    
    -- Generate column expressions with minimal data
    FOR i IN 1..max_cols LOOP
        IF i > 1 THEN
            insert_sql := insert_sql || ', ';
        END IF;
        
        -- Alternating pattern: INTEGER, TEXT (minimal data)
        IF (i % 2 = 1) THEN
            insert_sql := insert_sql || 'i';  -- Just the row number
        ELSE
            insert_sql := insert_sql || '''x''';  -- Single character
        END IF;
    END LOOP;
    
    insert_sql := insert_sql || ' FROM generate_series(1, 2000) i';
    
    RAISE NOTICE 'Executing INSERT for heap table...';
    EXECUTE insert_sql;
    
    RAISE NOTICE 'Copying to optimized table...';
    EXECUTE 'INSERT INTO test_optimized_extreme_width SELECT * FROM test_heap_extreme_width';
    
    RAISE NOTICE 'Inserted 2000 rows into both %-column tables successfully', max_cols;
END $$;

\echo '=== PERFORMANCE TEST: SELECT Last Column (col600) ==='

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
    RAISE NOTICE 'Testing LAST COLUMN (col600 TEXT) in 600-column table - 5 iterations with warmup...';
    
    -- WARMUP
    SELECT COUNT(*) INTO result_count FROM test_heap_extreme_width WHERE col600 = 'x';
    SELECT COUNT(*) INTO result_count FROM test_optimized_extreme_width WHERE col600 = 'x';
    
    -- Test heap format (5 runs)
    FOR i IN 1..5 LOOP
        start_time := clock_timestamp();
        SELECT COUNT(*) INTO result_count FROM test_heap_extreme_width WHERE col600 = 'x';
        end_time := clock_timestamp();
        duration_ms := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;
        heap_times := array_append(heap_times, duration_ms);
    END LOOP;
    
    -- Test optimized format (5 runs)
    FOR i IN 1..5 LOOP
        start_time := clock_timestamp();
        SELECT COUNT(*) INTO result_count FROM test_optimized_extreme_width WHERE col600 = 'x';
        end_time := clock_timestamp();
        duration_ms := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;
        optimized_times := array_append(optimized_times, duration_ms);
    END LOOP;
    
    -- Calculate statistics
    SELECT AVG(x) INTO heap_avg FROM unnest(heap_times) AS x;
    SELECT AVG(x) INTO optimized_avg FROM unnest(optimized_times) AS x;
    
    RAISE NOTICE 'EXTREME WIDTH - Last Column (col600 TEXT) Performance:';
    RAISE NOTICE '  Heap - Avg: % ms', ROUND(heap_avg, 3);
    RAISE NOTICE '  Optimized - Avg: % ms', ROUND(optimized_avg, 3);
    RAISE NOTICE '  Speedup: %x', ROUND(heap_avg / NULLIF(optimized_avg, 0), 3);
    RAISE NOTICE '  Result count: %', result_count;
    RAISE NOTICE '  This represents accessing the last column in an extremely wide table';
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
    SELECT COUNT(*) INTO result_count FROM test_heap_extreme_width WHERE col1 > 1000;
    end_time := clock_timestamp();
    heap_avg := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;
    
    start_time := clock_timestamp();
    SELECT COUNT(*) INTO result_count FROM test_optimized_extreme_width WHERE col1 > 1000;
    end_time := clock_timestamp();
    optimized_avg := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;
    
    RAISE NOTICE 'COMPARISON - First Column (col1 INTEGER) Performance:';
    RAISE NOTICE '  Heap: % ms, Optimized: % ms, Speedup: %x', 
                 ROUND(heap_avg, 3), ROUND(optimized_avg, 3), 
                 ROUND(heap_avg / NULLIF(optimized_avg, 0), 3);
    RAISE NOTICE '  Position impact: First vs Last column (1st vs 600th)';
END $$;

\echo '=== COMPARISON: Middle Column Performance ==='

DO $$
DECLARE
    heap_avg NUMERIC;
    optimized_avg NUMERIC;
    start_time TIMESTAMP;
    end_time TIMESTAMP;
    result_count INTEGER;
BEGIN
    -- Test middle column (col300) for comparison
    start_time := clock_timestamp();
    SELECT COUNT(*) INTO result_count FROM test_heap_extreme_width WHERE col300 = 'x';
    end_time := clock_timestamp();
    heap_avg := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;
    
    start_time := clock_timestamp();
    SELECT COUNT(*) INTO result_count FROM test_optimized_extreme_width WHERE col300 = 'x';
    end_time := clock_timestamp();
    optimized_avg := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;
    
    RAISE NOTICE 'MIDDLE COLUMN - col300 (TEXT) Performance:';
    RAISE NOTICE '  Heap: % ms, Optimized: % ms, Speedup: %x', 
                 ROUND(heap_avg, 3), ROUND(optimized_avg, 3), 
                 ROUND(heap_avg / NULLIF(optimized_avg, 0), 3);
    RAISE NOTICE '  Position: Middle column (300th of 600)';
END $$;

\echo '=== Storage Comparison ==='
SELECT 
    '600 Mixed Columns (Extreme Width)' as test_case,
    pg_size_pretty(pg_total_relation_size('test_heap_extreme_width')) as heap_size,
    pg_size_pretty(pg_total_relation_size('test_optimized_extreme_width')) as optimized_size,
    ROUND(
        (pg_total_relation_size('test_optimized_extreme_width')::NUMERIC / 
         pg_total_relation_size('test_heap_extreme_width')::NUMERIC - 1) * 100, 1
    ) || '%' as overhead_percent;

\echo '=== Extreme Width Performance Test Complete ==='
\echo 'This test demonstrates:'
\echo '- 600 columns with alternating INTEGER/TEXT pattern'
\echo '- Tests first, middle (300th), and last (600th) column access'
\echo '- Shows extreme column position impact on performance'
\echo '- Uses minimal data to fit within PostgreSQL row size limits'
\echo '- Represents the practical maximum width for mixed column types'
