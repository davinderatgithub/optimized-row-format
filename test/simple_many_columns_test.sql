-- Simple Many Columns Performance Test for Optimized Row Format
-- This test uses explicit transactions to avoid commit issues

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

\echo '=== Creating tables with 100 columns (50 fixed + 50 variable) ==='

-- Create heap table with 100 columns
DROP TABLE IF EXISTS heap_many_cols;
CREATE TABLE heap_many_cols (
    -- 50 fixed-length columns
    col1 INTEGER,
    col2 INTEGER,
    col3 INTEGER,
    col4 INTEGER,
    col5 INTEGER,
    col6 INTEGER,
    col7 INTEGER,
    col8 INTEGER,
    col9 INTEGER,
    col10 INTEGER,
    col11 INTEGER,
    col12 INTEGER,
    col13 INTEGER,
    col14 INTEGER,
    col15 INTEGER,
    col16 INTEGER,
    col17 INTEGER,
    col18 INTEGER,
    col19 INTEGER,
    col20 INTEGER,
    col21 INTEGER,
    col22 INTEGER,
    col23 INTEGER,
    col24 INTEGER,
    col25 INTEGER,
    col26 INTEGER,
    col27 INTEGER,
    col28 INTEGER,
    col29 INTEGER,
    col30 INTEGER,
    col31 INTEGER,
    col32 INTEGER,
    col33 INTEGER,
    col34 INTEGER,
    col35 INTEGER,
    col36 INTEGER,
    col37 INTEGER,
    col38 INTEGER,
    col39 INTEGER,
    col40 INTEGER,
    col41 INTEGER,
    col42 INTEGER,
    col43 INTEGER,
    col44 INTEGER,
    col45 INTEGER,
    col46 INTEGER,
    col47 INTEGER,
    col48 INTEGER,
    col49 INTEGER,
    col50 INTEGER,
    -- 50 variable-length columns
    col51 TEXT,
    col52 TEXT,
    col53 TEXT,
    col54 TEXT,
    col55 TEXT,
    col56 TEXT,
    col57 TEXT,
    col58 TEXT,
    col59 TEXT,
    col60 TEXT,
    col61 TEXT,
    col62 TEXT,
    col63 TEXT,
    col64 TEXT,
    col65 TEXT,
    col66 TEXT,
    col67 TEXT,
    col68 TEXT,
    col69 TEXT,
    col70 TEXT,
    col71 TEXT,
    col72 TEXT,
    col73 TEXT,
    col74 TEXT,
    col75 TEXT,
    col76 TEXT,
    col77 TEXT,
    col78 TEXT,
    col79 TEXT,
    col80 TEXT,
    col81 TEXT,
    col82 TEXT,
    col83 TEXT,
    col84 TEXT,
    col85 TEXT,
    col86 TEXT,
    col87 TEXT,
    col88 TEXT,
    col89 TEXT,
    col90 TEXT,
    col91 TEXT,
    col92 TEXT,
    col93 TEXT,
    col94 TEXT,
    col95 TEXT,
    col96 TEXT,
    col97 TEXT,
    col98 TEXT,
    col99 TEXT,
    col100 TEXT
);

-- Create optimized table with 100 columns
DROP TABLE IF EXISTS optimized_many_cols;
CREATE TABLE optimized_many_cols (
    -- 50 fixed-length columns
    col1 INTEGER,
    col2 INTEGER,
    col3 INTEGER,
    col4 INTEGER,
    col5 INTEGER,
    col6 INTEGER,
    col7 INTEGER,
    col8 INTEGER,
    col9 INTEGER,
    col10 INTEGER,
    col11 INTEGER,
    col12 INTEGER,
    col13 INTEGER,
    col14 INTEGER,
    col15 INTEGER,
    col16 INTEGER,
    col17 INTEGER,
    col18 INTEGER,
    col19 INTEGER,
    col20 INTEGER,
    col21 INTEGER,
    col22 INTEGER,
    col23 INTEGER,
    col24 INTEGER,
    col25 INTEGER,
    col26 INTEGER,
    col27 INTEGER,
    col28 INTEGER,
    col29 INTEGER,
    col30 INTEGER,
    col31 INTEGER,
    col32 INTEGER,
    col33 INTEGER,
    col34 INTEGER,
    col35 INTEGER,
    col36 INTEGER,
    col37 INTEGER,
    col38 INTEGER,
    col39 INTEGER,
    col40 INTEGER,
    col41 INTEGER,
    col42 INTEGER,
    col43 INTEGER,
    col44 INTEGER,
    col45 INTEGER,
    col46 INTEGER,
    col47 INTEGER,
    col48 INTEGER,
    col49 INTEGER,
    col50 INTEGER,
    -- 50 variable-length columns
    col51 TEXT,
    col52 TEXT,
    col53 TEXT,
    col54 TEXT,
    col55 TEXT,
    col56 TEXT,
    col57 TEXT,
    col58 TEXT,
    col59 TEXT,
    col60 TEXT,
    col61 TEXT,
    col62 TEXT,
    col63 TEXT,
    col64 TEXT,
    col65 TEXT,
    col66 TEXT,
    col67 TEXT,
    col68 TEXT,
    col69 TEXT,
    col70 TEXT,
    col71 TEXT,
    col72 TEXT,
    col73 TEXT,
    col74 TEXT,
    col75 TEXT,
    col76 TEXT,
    col77 TEXT,
    col78 TEXT,
    col79 TEXT,
    col80 TEXT,
    col81 TEXT,
    col82 TEXT,
    col83 TEXT,
    col84 TEXT,
    col85 TEXT,
    col86 TEXT,
    col87 TEXT,
    col88 TEXT,
    col89 TEXT,
    col90 TEXT,
    col91 TEXT,
    col92 TEXT,
    col93 TEXT,
    col94 TEXT,
    col95 TEXT,
    col96 TEXT,
    col97 TEXT,
    col98 TEXT,
    col99 TEXT,
    col100 TEXT
) USING optimized_row_format;

\echo '=== Test 1: INSERT Performance (1,000 rows with explicit commits) ==='

-- Insert performance test for heap table with explicit commits
BEGIN;
INSERT INTO heap_many_cols (
    col1, col2, col3, col4, col5, col6, col7, col8, col9, col10,
    col11, col12, col13, col14, col15, col16, col17, col18, col19, col20,
    col21, col22, col23, col24, col25, col26, col27, col28, col29, col30,
    col31, col32, col33, col34, col35, col36, col37, col38, col39, col40,
    col41, col42, col43, col44, col45, col46, col47, col48, col49, col50,
    col51, col52, col53, col54, col55, col56, col57, col58, col59, col60,
    col61, col62, col63, col64, col65, col66, col67, col68, col69, col70,
    col71, col72, col73, col74, col75, col76, col77, col78, col79, col80,
    col81, col82, col83, col84, col85, col86, col87, col88, col89, col90,
    col91, col92, col93, col94, col95, col96, col97, col98, col99, col100
)
SELECT
    i, i*2, i*3, i*4, i*5, i*6, i*7, i*8, i*9, i*10,
    i*11, i*12, i*13, i*14, i*15, i*16, i*17, i*18, i*19, i*20,
    i*21, i*22, i*23, i*24, i*25, i*26, i*27, i*28, i*29, i*30,
    i*31, i*32, i*33, i*34, i*35, i*36, i*37, i*38, i*39, i*40,
    i*41, i*42, i*43, i*44, i*45, i*46, i*47, i*48, i*49, i*50,
    'Text value ' || i, 'Text value ' || (i*2), 'Text value ' || (i*3), 'Text value ' || (i*4), 'Text value ' || (i*5),
    'Text value ' || (i*6), 'Text value ' || (i*7), 'Text value ' || (i*8), 'Text value ' || (i*9), 'Text value ' || (i*10),
    'Text value ' || (i*11), 'Text value ' || (i*12), 'Text value ' || (i*13), 'Text value ' || (i*14), 'Text value ' || (i*15),
    'Text value ' || (i*16), 'Text value ' || (i*17), 'Text value ' || (i*18), 'Text value ' || (i*19), 'Text value ' || (i*20),
    'Text value ' || (i*21), 'Text value ' || (i*22), 'Text value ' || (i*23), 'Text value ' || (i*24), 'Text value ' || (i*25),
    'Text value ' || (i*26), 'Text value ' || (i*27), 'Text value ' || (i*28), 'Text value ' || (i*29), 'Text value ' || (i*30),
    'Text value ' || (i*31), 'Text value ' || (i*32), 'Text value ' || (i*33), 'Text value ' || (i*34), 'Text value ' || (i*35),
    'Text value ' || (i*36), 'Text value ' || (i*37), 'Text value ' || (i*38), 'Text value ' || (i*39), 'Text value ' || (i*40),
    'Text value ' || (i*41), 'Text value ' || (i*42), 'Text value ' || (i*43), 'Text value ' || (i*44), 'Text value ' || (i*45),
    'Text value ' || (i*46), 'Text value ' || (i*47), 'Text value ' || (i*48), 'Text value ' || (i*49), 'Text value ' || (i*50)
FROM generate_series(1, 1000) i;
COMMIT;

SELECT 'Heap table INSERT completed' as status, COUNT(*) as row_count FROM heap_many_cols;

-- Insert performance test for optimized table with explicit commits
BEGIN;
INSERT INTO optimized_many_cols (
    col1, col2, col3, col4, col5, col6, col7, col8, col9, col10,
    col11, col12, col13, col14, col15, col16, col17, col18, col19, col20,
    col21, col22, col23, col24, col25, col26, col27, col28, col29, col30,
    col31, col32, col33, col34, col35, col36, col37, col38, col39, col40,
    col41, col42, col43, col44, col45, col46, col47, col48, col49, col50,
    col51, col52, col53, col54, col55, col56, col57, col58, col59, col60,
    col61, col62, col63, col64, col65, col66, col67, col68, col69, col70,
    col71, col72, col73, col74, col75, col76, col77, col78, col79, col80,
    col81, col82, col83, col84, col85, col86, col87, col88, col89, col90,
    col91, col92, col93, col94, col95, col96, col97, col98, col99, col100
)
SELECT
    i, i*2, i*3, i*4, i*5, i*6, i*7, i*8, i*9, i*10,
    i*11, i*12, i*13, i*14, i*15, i*16, i*17, i*18, i*19, i*20,
    i*21, i*22, i*23, i*24, i*25, i*26, i*27, i*28, i*29, i*30,
    i*31, i*32, i*33, i*34, i*35, i*36, i*37, i*38, i*39, i*40,
    i*41, i*42, i*43, i*44, i*45, i*46, i*47, i*48, i*49, i*50,
    'Text value ' || i, 'Text value ' || (i*2), 'Text value ' || (i*3), 'Text value ' || (i*4), 'Text value ' || (i*5),
    'Text value ' || (i*6), 'Text value ' || (i*7), 'Text value ' || (i*8), 'Text value ' || (i*9), 'Text value ' || (i*10),
    'Text value ' || (i*11), 'Text value ' || (i*12), 'Text value ' || (i*13), 'Text value ' || (i*14), 'Text value ' || (i*15),
    'Text value ' || (i*16), 'Text value ' || (i*17), 'Text value ' || (i*18), 'Text value ' || (i*19), 'Text value ' || (i*20),
    'Text value ' || (i*21), 'Text value ' || (i*22), 'Text value ' || (i*23), 'Text value ' || (i*24), 'Text value ' || (i*25),
    'Text value ' || (i*26), 'Text value ' || (i*27), 'Text value ' || (i*28), 'Text value ' || (i*29), 'Text value ' || (i*30),
    'Text value ' || (i*31), 'Text value ' || (i*32), 'Text value ' || (i*33), 'Text value ' || (i*34), 'Text value ' || (i*35),
    'Text value ' || (i*36), 'Text value ' || (i*37), 'Text value ' || (i*38), 'Text value ' || (i*39), 'Text value ' || (i*40),
    'Text value ' || (i*41), 'Text value ' || (i*42), 'Text value ' || (i*43), 'Text value ' || (i*44), 'Text value ' || (i*45),
    'Text value ' || (i*46), 'Text value ' || (i*47), 'Text value ' || (i*48), 'Text value ' || (i*49), 'Text value ' || (i*50)
FROM generate_series(1, 1000) i;
COMMIT;

SELECT 'Optimized table INSERT completed' as status, COUNT(*) as row_count FROM optimized_many_cols;

\echo '=== Test 2: Single Column SELECT Performance ==='

-- Test single column selection for fixed-length columns
SELECT 'SELECT heap_many_cols.col10 (fixed-length)' as test, COUNT(*) as result FROM heap_many_cols WHERE col10 > 500;
SELECT 'SELECT optimized_many_cols.col10 (fixed-length)' as test, COUNT(*) as result FROM optimized_many_cols WHERE col10 > 500;

SELECT 'SELECT heap_many_cols.col20 (fixed-length)' as test, COUNT(*) as result FROM heap_many_cols WHERE col20 > 1000;
SELECT 'SELECT optimized_many_cols.col20 (fixed-length)' as test, COUNT(*) as result FROM optimized_many_cols WHERE col20 > 1000;

SELECT 'SELECT heap_many_cols.col30 (fixed-length)' as test, COUNT(*) as result FROM heap_many_cols WHERE col30 > 1500;
SELECT 'SELECT optimized_many_cols.col30 (fixed-length)' as test, COUNT(*) as result FROM optimized_many_cols WHERE col30 > 1500;

-- Test single column selection for variable-length columns
SELECT 'SELECT heap_many_cols.col60 (variable-length)' as test, COUNT(*) as result FROM heap_many_cols WHERE col60 LIKE '%100%';
SELECT 'SELECT optimized_many_cols.col60 (variable-length)' as test, COUNT(*) as result FROM optimized_many_cols WHERE col60 LIKE '%100%';

SELECT 'SELECT heap_many_cols.col70 (variable-length)' as test, COUNT(*) as result FROM heap_many_cols WHERE col70 LIKE '%200%';
SELECT 'SELECT optimized_many_cols.col70 (variable-length)' as test, COUNT(*) as result FROM optimized_many_cols WHERE col70 LIKE '%200%';

SELECT 'SELECT heap_many_cols.col80 (variable-length)' as test, COUNT(*) as result FROM heap_many_cols WHERE col80 LIKE '%300%';
SELECT 'SELECT optimized_many_cols.col80 (variable-length)' as test, COUNT(*) as result FROM optimized_many_cols WHERE col80 LIKE '%300%';

\echo '=== Test 3: Storage Size Comparison ==='

-- Compare storage sizes
SELECT
    'Heap Table' as table_type,
    pg_size_pretty(pg_total_relation_size('heap_many_cols')) as total_size,
    pg_size_pretty(pg_relation_size('heap_many_cols')) as table_size,
    pg_size_pretty(pg_total_relation_size('heap_many_cols') - pg_relation_size('heap_many_cols')) as index_size
UNION ALL
SELECT
    'Optimized Table' as table_type,
    pg_size_pretty(pg_total_relation_size('optimized_many_cols')) as total_size,
    pg_size_pretty(pg_relation_size('optimized_many_cols')) as table_size,
    pg_size_pretty(pg_total_relation_size('optimized_many_cols') - pg_relation_size('optimized_many_cols')) as index_size;

-- Show row counts
SELECT
    'Heap Table' as table_type,
    COUNT(*) as row_count
FROM heap_many_cols
UNION ALL
SELECT
    'Optimized Table' as table_type,
    COUNT(*) as row_count
FROM optimized_many_cols;

\echo '=== Test Complete ==='