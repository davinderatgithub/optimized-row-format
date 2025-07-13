-- Benchmark script for Optimized Row Format vs Standard Heap Format
-- This script tests various scenarios to measure performance improvements

-- Enable timing for accurate measurements
\timing on

-- Create the extension if it doesn't exist
\echo '=== Creating optimized_row_format extension ==='
CREATE EXTENSION IF NOT EXISTS optimized_row_format;

-- Verify extension is available
\echo '=== Verifying extension availability ==='
SELECT
    CASE
        WHEN EXISTS (
            SELECT 1 FROM pg_am WHERE amname = 'optimized_row_format'
        ) THEN '✅ Extension is available'
        ELSE '❌ Extension is NOT available - tests will fail'
    END as extension_status;

-- Create test database and tables
\echo '=== Setting up test environment ==='

-- Create test tables with different column patterns
CREATE TABLE test_heap (
    id SERIAL PRIMARY KEY,
    small_int SMALLINT,
    regular_int INTEGER,
    big_int BIGINT,
    text_col TEXT,
    varchar_col VARCHAR(100),
    float_col REAL,
    double_col DOUBLE PRECISION,
    bool_col BOOLEAN,
    date_col DATE,
    timestamp_col TIMESTAMP,
    json_col JSONB
);

CREATE TABLE test_optimized (
    id SERIAL PRIMARY KEY,
    small_int SMALLINT,
    regular_int INTEGER,
    big_int BIGINT,
    text_col TEXT,
    varchar_col VARCHAR(100),
    float_col REAL,
    double_col DOUBLE PRECISION,
    bool_col BOOLEAN,
    date_col DATE,
    timestamp_col TIMESTAMP,
    json_col JSONB
) USING optimized_row_format;

-- Create tables with different null patterns
CREATE TABLE test_heap_nulls (
    id SERIAL PRIMARY KEY,
    col1 INTEGER,
    col2 TEXT,
    col3 INTEGER,
    col4 TEXT,
    col5 INTEGER,
    col6 TEXT,
    col7 INTEGER,
    col8 TEXT
);

CREATE TABLE test_optimized_nulls (
    id SERIAL PRIMARY KEY,
    col1 INTEGER,
    col2 TEXT,
    col3 INTEGER,
    col4 TEXT,
    col5 INTEGER,
    col6 TEXT,
    col7 INTEGER,
    col8 TEXT
) USING optimized_row_format;

-- Create tables with mostly fixed-length columns
CREATE TABLE test_heap_fixed (
    id SERIAL PRIMARY KEY,
    int1 INTEGER,
    int2 INTEGER,
    int3 INTEGER,
    int4 INTEGER,
    int5 INTEGER,
    int6 INTEGER,
    int7 INTEGER,
    int8 INTEGER,
    small1 SMALLINT,
    small2 SMALLINT,
    bool1 BOOLEAN,
    bool2 BOOLEAN,
    float1 REAL,
    float2 REAL
);

CREATE TABLE test_optimized_fixed (
    id SERIAL PRIMARY KEY,
    int1 INTEGER,
    int2 INTEGER,
    int3 INTEGER,
    int4 INTEGER,
    int5 INTEGER,
    int6 INTEGER,
    int7 INTEGER,
    int8 INTEGER,
    small1 SMALLINT,
    small2 SMALLINT,
    bool1 BOOLEAN,
    bool2 BOOLEAN,
    float1 REAL,
    float2 REAL
) USING optimized_row_format;

-- Create tables with mostly variable-length columns
CREATE TABLE test_heap_var (
    id SERIAL PRIMARY KEY,
    text1 TEXT,
    text2 TEXT,
    text3 TEXT,
    text4 TEXT,
    text5 TEXT,
    varchar1 VARCHAR(200),
    varchar2 VARCHAR(200),
    varchar3 VARCHAR(200),
    json1 JSONB,
    json2 JSONB,
    json3 JSONB
);

CREATE TABLE test_optimized_var (
    id SERIAL PRIMARY KEY,
    text1 TEXT,
    text2 TEXT,
    text3 TEXT,
    text4 TEXT,
    text5 TEXT,
    varchar1 VARCHAR(200),
    varchar2 VARCHAR(200),
    varchar3 VARCHAR(200),
    json1 JSONB,
    json2 JSONB,
    json3 JSONB
) USING optimized_row_format;

\echo '=== Test 1: INSERT Performance (Mixed Data Types) ==='

-- Insert performance test with mixed data types
DO $$
DECLARE
    start_time TIMESTAMP;
    end_time TIMESTAMP;
    heap_time INTERVAL;
    optimized_time INTERVAL;
    i INTEGER;
BEGIN
    -- Test heap format
    start_time := clock_timestamp();
    FOR i IN 1..10000 LOOP
        INSERT INTO test_heap (small_int, regular_int, big_int, text_col, varchar_col,
                              float_col, double_col, bool_col, date_col, timestamp_col, json_col)
        VALUES (
            (i % 32767)::SMALLINT,
            i,
            i::BIGINT * 1000,
            'This is a test text column with some content for row ' || i,
            'Varchar content ' || i,
            i * 1.5,
            i * 2.5,
            (i % 2)::BOOLEAN,
            CURRENT_DATE + (i % 365),
            CURRENT_TIMESTAMP + (i || ' seconds')::INTERVAL,
            '{"key": "value", "number": ' || i || ', "array": [1,2,3]}'::JSONB
        );
    END LOOP;
    end_time := clock_timestamp();
    heap_time := end_time - start_time;

    -- Test optimized format
    start_time := clock_timestamp();
    FOR i IN 1..10000 LOOP
        INSERT INTO test_optimized (small_int, regular_int, big_int, text_col, varchar_col,
                                   float_col, double_col, bool_col, date_col, timestamp_col, json_col)
        VALUES (
            (i % 32767)::SMALLINT,
            i,
            i::BIGINT * 1000,
            'This is a test text column with some content for row ' || i,
            'Varchar content ' || i,
            i * 1.5,
            i * 2.5,
            (i % 2)::BOOLEAN,
            CURRENT_DATE + (i % 365),
            CURRENT_TIMESTAMP + (i || ' seconds')::INTERVAL,
            '{"key": "value", "number": ' || i || ', "array": [1,2,3]}'::JSONB
        );
    END LOOP;
    end_time := clock_timestamp();
    optimized_time := end_time - start_time;

    RAISE NOTICE 'INSERT Performance (10,000 rows):';
    RAISE NOTICE 'Heap format: %', heap_time;
    RAISE NOTICE 'Optimized format: %', optimized_time;
    RAISE NOTICE 'Speedup: %x', EXTRACT(EPOCH FROM heap_time) / EXTRACT(EPOCH FROM optimized_time);
END $$;

\echo '=== Test 2: SELECT Performance (Fixed-Length Columns) ==='

-- Test SELECT performance on fixed-length columns
DO $$
DECLARE
    start_time TIMESTAMP;
    end_time TIMESTAMP;
    heap_time INTERVAL;
    optimized_time INTERVAL;
    result_count INTEGER;
BEGIN
    -- Test heap format
    start_time := clock_timestamp();
    SELECT COUNT(*) INTO result_count FROM test_heap WHERE regular_int % 2 = 0;
    end_time := clock_timestamp();
    heap_time := end_time - start_time;

    -- Test optimized format
    start_time := clock_timestamp();
    SELECT COUNT(*) INTO result_count FROM test_optimized WHERE regular_int % 2 = 0;
    end_time := clock_timestamp();
    optimized_time := end_time - start_time;

    RAISE NOTICE 'SELECT Performance (Fixed-length columns):';
    RAISE NOTICE 'Heap format: %', heap_time;
    RAISE NOTICE 'Optimized format: %', optimized_time;
    RAISE NOTICE 'Speedup: %x', EXTRACT(EPOCH FROM heap_time) / EXTRACT(EPOCH FROM optimized_time);
END $$;

\echo '=== Test 3: SELECT Performance (Variable-Length Columns) ==='

-- Test SELECT performance on variable-length columns
DO $$
DECLARE
    start_time TIMESTAMP;
    end_time TIMESTAMP;
    heap_time INTERVAL;
    optimized_time INTERVAL;
    result_count INTEGER;
BEGIN
    -- Test heap format
    start_time := clock_timestamp();
    SELECT COUNT(*) INTO result_count FROM test_heap WHERE text_col LIKE '%test%';
    end_time := clock_timestamp();
    heap_time := end_time - start_time;

    -- Test optimized format
    start_time := clock_timestamp();
    SELECT COUNT(*) INTO result_count FROM test_optimized WHERE text_col LIKE '%test%';
    end_time := clock_timestamp();
    optimized_time := end_time - start_time;

    RAISE NOTICE 'SELECT Performance (Variable-length columns):';
    RAISE NOTICE 'Heap format: %', heap_time;
    RAISE NOTICE 'Optimized format: %', optimized_time;
    RAISE NOTICE 'Speedup: %x', EXTRACT(EPOCH FROM heap_time) / EXTRACT(EPOCH FROM optimized_time);
END $$;

\echo '=== Test 4: NULL Handling Performance ==='

-- Insert data with various null patterns
INSERT INTO test_heap_nulls (col1, col2, col3, col4, col5, col6, col7, col8)
SELECT
    CASE WHEN i % 3 = 0 THEN NULL ELSE i END,
    CASE WHEN i % 4 = 0 THEN NULL ELSE 'text' || i END,
    CASE WHEN i % 5 = 0 THEN NULL ELSE i * 2 END,
    CASE WHEN i % 6 = 0 THEN NULL ELSE 'varchar' || i END,
    CASE WHEN i % 7 = 0 THEN NULL ELSE i * 3 END,
    CASE WHEN i % 8 = 0 THEN NULL ELSE 'more text' || i END,
    CASE WHEN i % 9 = 0 THEN NULL ELSE i * 4 END,
    CASE WHEN i % 10 = 0 THEN NULL ELSE 'final text' || i END
FROM generate_series(1, 5000) i;

INSERT INTO test_optimized_nulls (col1, col2, col3, col4, col5, col6, col7, col8)
SELECT
    CASE WHEN i % 3 = 0 THEN NULL ELSE i END,
    CASE WHEN i % 4 = 0 THEN NULL ELSE 'text' || i END,
    CASE WHEN i % 5 = 0 THEN NULL ELSE i * 2 END,
    CASE WHEN i % 6 = 0 THEN NULL ELSE 'varchar' || i END,
    CASE WHEN i % 7 = 0 THEN NULL ELSE i * 3 END,
    CASE WHEN i % 8 = 0 THEN NULL ELSE 'more text' || i END,
    CASE WHEN i % 9 = 0 THEN NULL ELSE i * 4 END,
    CASE WHEN i % 10 = 0 THEN NULL ELSE 'final text' || i END
FROM generate_series(1, 5000) i;

-- Test NULL checking performance
DO $$
DECLARE
    start_time TIMESTAMP;
    end_time TIMESTAMP;
    heap_time INTERVAL;
    optimized_time INTERVAL;
    result_count INTEGER;
BEGIN
    -- Test heap format
    start_time := clock_timestamp();
    SELECT COUNT(*) INTO result_count FROM test_heap_nulls WHERE col1 IS NULL OR col3 IS NULL;
    end_time := clock_timestamp();
    heap_time := end_time - start_time;

    -- Test optimized format
    start_time := clock_timestamp();
    SELECT COUNT(*) INTO result_count FROM test_optimized_nulls WHERE col1 IS NULL OR col3 IS NULL;
    end_time := clock_timestamp();
    optimized_time := end_time - start_time;

    RAISE NOTICE 'NULL Checking Performance:';
    RAISE NOTICE 'Heap format: %', heap_time;
    RAISE NOTICE 'Optimized format: %', optimized_time;
    RAISE NOTICE 'Speedup: %x', EXTRACT(EPOCH FROM heap_time) / EXTRACT(EPOCH FROM optimized_time);
END $$;

\echo '=== Test 5: Storage Efficiency ==='

-- Compare storage sizes
SELECT
    schemaname,
    tablename,
    pg_size_pretty(pg_total_relation_size(schemaname||'.'||tablename)) as total_size,
    pg_size_pretty(pg_relation_size(schemaname||'.'||tablename)) as table_size,
    pg_size_pretty(pg_total_relation_size(schemaname||'.'||tablename) - pg_relation_size(schemaname||'.'||tablename)) as index_size
FROM pg_tables
WHERE tablename LIKE 'test_%'
ORDER BY tablename;

\echo '=== Test 6: Fixed-Length Column Performance ==='

-- Insert data into fixed-length tables
INSERT INTO test_heap_fixed (int1, int2, int3, int4, int5, int6, int7, int8, small1, small2, bool1, bool2, float1, float2)
SELECT i, i*2, i*3, i*4, i*5, i*6, i*7, i*8, (i%32767)::SMALLINT, (i*2%32767)::SMALLINT,
       (i%2)::BOOLEAN, ((i+1)%2)::BOOLEAN, i*1.5, i*2.5
FROM generate_series(1, 10000) i;

INSERT INTO test_optimized_fixed (int1, int2, int3, int4, int5, int6, int7, int8, small1, small2, bool1, bool2, float1, float2)
SELECT i, i*2, i*3, i*4, i*5, i*6, i*7, i*8, (i%32767)::SMALLINT, (i*2%32767)::SMALLINT,
       (i%2)::BOOLEAN, ((i+1)%2)::BOOLEAN, i*1.5, i*2.5
FROM generate_series(1, 10000) i;

-- Test fixed-length column access
DO $$
DECLARE
    start_time TIMESTAMP;
    end_time TIMESTAMP;
    heap_time INTERVAL;
    optimized_time INTERVAL;
    result_count INTEGER;
BEGIN
    -- Test heap format
    start_time := clock_timestamp();
    SELECT COUNT(*) INTO result_count FROM test_heap_fixed WHERE int1 > 5000 AND int2 < 15000;
    end_time := clock_timestamp();
    heap_time := end_time - start_time;

    -- Test optimized format
    start_time := clock_timestamp();
    SELECT COUNT(*) INTO result_count FROM test_optimized_fixed WHERE int1 > 5000 AND int2 < 15000;
    end_time := clock_timestamp();
    optimized_time := end_time - start_time;

    RAISE NOTICE 'Fixed-Length Column Access Performance:';
    RAISE NOTICE 'Heap format: %', heap_time;
    RAISE NOTICE 'Optimized format: %', optimized_time;
    RAISE NOTICE 'Speedup: %x', EXTRACT(EPOCH FROM heap_time) / EXTRACT(EPOCH FROM optimized_time);
END $$;

\echo '=== Test 7: Variable-Length Column Performance ==='

-- Insert data into variable-length tables
INSERT INTO test_heap_var (text1, text2, text3, text4, text5, varchar1, varchar2, varchar3, json1, json2, json3)
SELECT
    'This is a long text field with some content for row ' || i,
    'Another text field with different content for row ' || i,
    'Third text field with more content for row ' || i,
    'Fourth text field with even more content for row ' || i,
    'Fifth text field with the most content for row ' || i,
    'Varchar field 1 for row ' || i,
    'Varchar field 2 for row ' || i,
    'Varchar field 3 for row ' || i,
    ('{"id": ' || i || ', "name": "item' || i || '", "tags": ["tag1", "tag2"]}')::JSONB,
    ('{"value": ' || i || ', "metadata": {"created": "2023-01-01", "updated": "2023-12-31"}}')::JSONB,
    ('{"data": {"field1": "value1", "field2": "value2", "count": ' || i || '}}')::JSONB
FROM generate_series(1, 5000) i;

INSERT INTO test_optimized_var (text1, text2, text3, text4, text5, varchar1, varchar2, varchar3, json1, json2, json3)
SELECT
    'This is a long text field with some content for row ' || i,
    'Another text field with different content for row ' || i,
    'Third text field with more content for row ' || i,
    'Fourth text field with even more content for row ' || i,
    'Fifth text field with the most content for row ' || i,
    'Varchar field 1 for row ' || i,
    'Varchar field 2 for row ' || i,
    'Varchar field 3 for row ' || i,
    ('{"id": ' || i || ', "name": "item' || i || '", "tags": ["tag1", "tag2"]}')::JSONB,
    ('{"value": ' || i || ', "metadata": {"created": "2023-01-01", "updated": "2023-12-31"}}')::JSONB,
    ('{"data": {"field1": "value1", "field2": "value2", "count": ' || i || '}}')::JSONB
FROM generate_series(1, 5000) i;

-- Test variable-length column access
DO $$
DECLARE
    start_time TIMESTAMP;
    end_time TIMESTAMP;
    heap_time INTERVAL;
    optimized_time INTERVAL;
    result_count INTEGER;
BEGIN
    -- Test heap format
    start_time := clock_timestamp();
    SELECT COUNT(*) INTO result_count FROM test_heap_var WHERE text1 LIKE '%long%' AND json1->>'id'::text = '100';
    end_time := clock_timestamp();
    heap_time := end_time - start_time;

    -- Test optimized format
    start_time := clock_timestamp();
    SELECT COUNT(*) INTO result_count FROM test_optimized_var WHERE text1 LIKE '%long%' AND json1->>'id'::text = '100';
    end_time := clock_timestamp();
    optimized_time := end_time - start_time;

    RAISE NOTICE 'Variable-Length Column Access Performance:';
    RAISE NOTICE 'Heap format: %', heap_time;
    RAISE NOTICE 'Optimized format: %', optimized_time;
    RAISE NOTICE 'Speedup: %x', EXTRACT(EPOCH FROM heap_time) / EXTRACT(EPOCH FROM optimized_time);
END $$;

\echo '=== Test 8: Full Table Scan Performance ==='

-- Test full table scan performance
DO $$
DECLARE
    start_time TIMESTAMP;
    end_time TIMESTAMP;
    heap_time INTERVAL;
    optimized_time INTERVAL;
    result_count INTEGER;
BEGIN
    -- Test heap format
    start_time := clock_timestamp();
    SELECT COUNT(*) INTO result_count FROM test_heap;
    end_time := clock_timestamp();
    heap_time := end_time - start_time;

    -- Test optimized format
    start_time := clock_timestamp();
    SELECT COUNT(*) INTO result_count FROM test_optimized;
    end_time := clock_timestamp();
    optimized_time := end_time - start_time;

    RAISE NOTICE 'Full Table Scan Performance:';
    RAISE NOTICE 'Heap format: %', heap_time;
    RAISE NOTICE 'Optimized format: %', optimized_time;
    RAISE NOTICE 'Speedup: %x', EXTRACT(EPOCH FROM heap_time) / EXTRACT(EPOCH FROM optimized_time);
END $$;

\echo '=== Final Storage Comparison ==='

-- Final storage comparison
SELECT
    'Heap Tables' as category,
    COUNT(*) as table_count,
    pg_size_pretty(SUM(pg_total_relation_size(schemaname||'.'||tablename))) as total_size
FROM pg_tables
WHERE tablename LIKE 'test_heap%'
UNION ALL
SELECT
    'Optimized Tables' as category,
    COUNT(*) as table_count,
    pg_size_pretty(SUM(pg_total_relation_size(schemaname||'.'||tablename))) as total_size
FROM pg_tables
WHERE tablename LIKE 'test_optimized%';

\echo '=== Benchmark Complete ==='