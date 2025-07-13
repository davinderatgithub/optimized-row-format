-- Debug script for Many Columns Test Issue
-- This script helps investigate why tables show size but 0 rows

\echo '=== Debugging Many Columns Test Issue ==='

-- Check if we're in a transaction
SELECT
    CASE
        WHEN txid_current() IS NOT NULL THEN 'In transaction: ' || txid_current()
        ELSE 'Not in transaction'
    END as transaction_status;

-- Check table existence and basic info
\echo ''
\echo '=== Table Information ==='
SELECT
    schemaname,
    tablename,
    tableowner,
    tablespace,
    hasindexes,
    hasrules,
    hastriggers,
    rowsecurity
FROM pg_tables
WHERE tablename IN ('heap_many_cols', 'optimized_many_cols')
ORDER BY tablename;

-- Check table statistics
\echo ''
\echo '=== Table Statistics ==='
SELECT
    schemaname,
    tablename,
    n_tup_ins as inserts,
    n_tup_upd as updates,
    n_tup_del as deletes,
    n_live_tup as live_tuples,
    n_dead_tup as dead_tuples,
    last_vacuum,
    last_autovacuum
FROM pg_stat_user_tables
WHERE tablename IN ('heap_many_cols', 'optimized_many_cols')
ORDER BY tablename;

-- Check table sizes
\echo ''
\echo '=== Table Sizes ==='
SELECT
    schemaname,
    tablename,
    pg_size_pretty(pg_total_relation_size(schemaname||'.'||tablename)) as total_size,
    pg_size_pretty(pg_relation_size(schemaname||'.'||tablename)) as table_size,
    pg_size_pretty(pg_total_relation_size(schemaname||'.'||tablename) - pg_relation_size(schemaname||'.'||tablename)) as index_size
FROM pg_tables
WHERE tablename IN ('heap_many_cols', 'optimized_many_cols')
ORDER BY tablename;

-- Check row counts
\echo ''
\echo '=== Row Counts ==='
SELECT 'heap_many_cols' as table_name, COUNT(*) as row_count FROM heap_many_cols
UNION ALL
SELECT 'optimized_many_cols' as table_name, COUNT(*) as row_count FROM optimized_many_cols;

-- Try to insert a single row and see what happens
\echo ''
\echo '=== Testing Single Row Insert ==='

-- Test heap table
\echo 'Testing heap table insert...'
BEGIN;
INSERT INTO heap_many_cols (col1, col2, col3, col4, col5, col6, col7, col8, col9, col10,
                           col11, col12, col13, col14, col15, col16, col17, col18, col19, col20,
                           col21, col22, col23, col24, col25, col26, col27, col28, col29, col30,
                           col31, col32, col33, col34, col35, col36, col37, col38, col39, col40,
                           col41, col42, col43, col44, col45, col46, col47, col48, col49, col50,
                           col51, col52, col53, col54, col55, col56, col57, col58, col59, col60,
                           col61, col62, col63, col64, col65, col66, col67, col68, col69, col70,
                           col71, col72, col73, col74, col75, col76, col77, col78, col79, col80,
                           col81, col82, col83, col84, col85, col86, col87, col88, col89, col90,
                           col91, col92, col93, col94, col95, col96, col97, col98, col99, col100)
VALUES (1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
        11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
        21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
        31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50,
        'text1', 'text2', 'text3', 'text4', 'text5',
        'text6', 'text7', 'text8', 'text9', 'text10',
        'text11', 'text12', 'text13', 'text14', 'text15',
        'text16', 'text17', 'text18', 'text19', 'text20',
        'text21', 'text22', 'text23', 'text24', 'text25',
        'text26', 'text27', 'text28', 'text29', 'text30',
        'text31', 'text32', 'text33', 'text34', 'text35',
        'text36', 'text37', 'text38', 'text39', 'text40',
        'text41', 'text42', 'text43', 'text44', 'text45',
        'text46', 'text47', 'text48', 'text49', 'text50');

SELECT 'After heap insert' as step, COUNT(*) as row_count FROM heap_many_cols;
COMMIT;
SELECT 'After heap commit' as step, COUNT(*) as row_count FROM heap_many_cols;

-- Test optimized table
\echo 'Testing optimized table insert...'
BEGIN;
INSERT INTO optimized_many_cols (col1, col2, col3, col4, col5, col6, col7, col8, col9, col10,
                                col11, col12, col13, col14, col15, col16, col17, col18, col19, col20,
                                col21, col22, col23, col24, col25, col26, col27, col28, col29, col30,
                                col31, col32, col33, col34, col35, col36, col37, col38, col39, col40,
                                col41, col42, col43, col44, col45, col46, col47, col48, col49, col50,
                                col51, col52, col53, col54, col55, col56, col57, col58, col59, col60,
                                col61, col62, col63, col64, col65, col66, col67, col68, col69, col70,
                                col71, col72, col73, col74, col75, col76, col77, col78, col79, col80,
                                col81, col82, col83, col84, col85, col86, col87, col88, col89, col90,
                                col91, col92, col93, col94, col95, col96, col97, col98, col99, col100)
VALUES (1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
        11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
        21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
        31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50,
        'text1', 'text2', 'text3', 'text4', 'text5',
        'text6', 'text7', 'text8', 'text9', 'text10',
        'text11', 'text12', 'text13', 'text14', 'text15',
        'text16', 'text17', 'text18', 'text19', 'text20',
        'text21', 'text22', 'text23', 'text24', 'text25',
        'text26', 'text27', 'text28', 'text29', 'text30',
        'text31', 'text32', 'text33', 'text34', 'text35',
        'text36', 'text37', 'text38', 'text39', 'text40',
        'text41', 'text42', 'text43', 'text44', 'text45',
        'text46', 'text47', 'text48', 'text49', 'text50');

SELECT 'After optimized insert' as step, COUNT(*) as row_count FROM optimized_many_cols;
COMMIT;
SELECT 'After optimized commit' as step, COUNT(*) as row_count FROM optimized_many_cols;

-- Check if we can select from the tables
\echo ''
\echo '=== Testing SELECT Operations ==='
SELECT 'heap_many_cols' as table_name, col1, col51 FROM heap_many_cols LIMIT 1;
SELECT 'optimized_many_cols' as table_name, col1, col51 FROM optimized_many_cols LIMIT 1;

-- Check for any errors in the logs
\echo ''
\echo '=== Extension Status ==='
SELECT
    CASE
        WHEN EXISTS (
            SELECT 1 FROM pg_am WHERE amname = 'optimized_row_format'
        ) THEN '✅ Extension is available'
        ELSE '❌ Extension is NOT available'
    END as extension_status;

-- Check extension functions
\echo ''
\echo '=== Extension Functions ==='
SELECT
    proname,
    prosrc IS NOT NULL as has_source
FROM pg_proc
WHERE proname LIKE '%optimized%'
ORDER BY proname;

\echo ''
\echo '=== Debug Complete ==='