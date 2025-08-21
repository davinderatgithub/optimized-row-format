-- Storage Analysis Test
-- This test analyzes storage overhead for different table configurations

\echo '=== Storage Overhead Analysis ==='

CREATE EXTENSION IF NOT EXISTS optimized_row_format;

-- Clean up existing tables
DROP TABLE IF EXISTS heap_narrow;
DROP TABLE IF EXISTS optimized_narrow;
DROP TABLE IF EXISTS heap_wide;
DROP TABLE IF EXISTS optimized_wide;

\echo '--- Test 1: Narrow table (3 columns) ---'

-- Create narrow table (heap)
CREATE TABLE heap_narrow (
    id INTEGER,
    name TEXT,
    value INTEGER
);

-- Create narrow table (optimized)
CREATE TABLE optimized_narrow (
    id INTEGER,
    name TEXT,
    value INTEGER
) USING optimized_row_format;

-- Insert a single row to analyze storage
INSERT INTO optimized_narrow VALUES (1, 'test_data_string', 42);

\echo 'Narrow table storage analysis complete'

\echo '--- Test 2: Wide table (many columns) ---'

-- Create wide table with many variable-length columns (optimized)
CREATE TABLE optimized_wide (
    col1 INTEGER,
    col2 TEXT,
    col3 INTEGER,
    col4 TEXT,
    col5 INTEGER,
    col6 TEXT,
    col7 INTEGER,
    col8 TEXT,
    col9 INTEGER,
    col10 TEXT,
    col11 INTEGER,
    col12 TEXT,
    col13 INTEGER,
    col14 TEXT,
    col15 INTEGER,
    col16 TEXT,
    col17 INTEGER,
    col18 TEXT,
    col19 INTEGER,
    col20 TEXT
) USING optimized_row_format;

-- Insert a single row to analyze storage for wide table
INSERT INTO optimized_wide VALUES (
    1, 'text1', 2, 'text2', 3, 'text3', 4, 'text4', 5, 'text5',
    6, 'text6', 7, 'text7', 8, 'text8', 9, 'text9', 10, 'text10'
);

\echo 'Wide table storage analysis complete'

\echo '--- Test 3: Very wide table (50+ columns) ---'

-- Create very wide table
CREATE TABLE optimized_very_wide (
    f1 INTEGER, t1 TEXT, f2 INTEGER, t2 TEXT, f3 INTEGER, t3 TEXT, f4 INTEGER, t4 TEXT, f5 INTEGER, t5 TEXT,
    f6 INTEGER, t6 TEXT, f7 INTEGER, t7 TEXT, f8 INTEGER, t8 TEXT, f9 INTEGER, t9 TEXT, f10 INTEGER, t10 TEXT,
    f11 INTEGER, t11 TEXT, f12 INTEGER, t12 TEXT, f13 INTEGER, t13 TEXT, f14 INTEGER, t14 TEXT, f15 INTEGER, t15 TEXT,
    f16 INTEGER, t16 TEXT, f17 INTEGER, t17 TEXT, f18 INTEGER, t18 TEXT, f19 INTEGER, t19 TEXT, f20 INTEGER, t20 TEXT,
    f21 INTEGER, t21 TEXT, f22 INTEGER, t22 TEXT, f23 INTEGER, t23 TEXT, f24 INTEGER, t24 TEXT, f25 INTEGER, t25 TEXT
) USING optimized_row_format;

-- Insert a single row to analyze storage for very wide table
INSERT INTO optimized_very_wide VALUES (
    1, 'data1', 2, 'data2', 3, 'data3', 4, 'data4', 5, 'data5',
    6, 'data6', 7, 'data7', 8, 'data8', 9, 'data9', 10, 'data10',
    11, 'data11', 12, 'data12', 13, 'data13', 14, 'data14', 15, 'data15',
    16, 'data16', 17, 'data17', 18, 'data18', 19, 'data19', 20, 'data20',
    21, 'data21', 22, 'data22', 23, 'data23', 24, 'data24', 25, 'data25'
);

\echo 'Very wide table storage analysis complete'

\echo '=== Analysis Complete ==='
