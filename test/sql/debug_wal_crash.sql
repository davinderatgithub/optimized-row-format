-- Debug WAL crash issue with detailed logging
\timing on

-- Enable detailed logging
SET log_min_messages = NOTICE;
SET log_statement = 'all';

\echo '=== WAL Crash Debug Test ==='

-- Clean up
DROP TABLE IF EXISTS debug_wal_heap;
DROP TABLE IF EXISTS debug_wal_optimized;

-- Create test tables
CREATE TABLE debug_wal_heap (
    id INTEGER,
    text_col TEXT,
    bool_col BOOLEAN,
    date_col TIMESTAMP
);

CREATE TABLE debug_wal_optimized (
    id INTEGER,
    text_col TEXT,
    bool_col BOOLEAN,
    date_col TIMESTAMP
) USING optimized_row_format;

\echo '=== Phase 1: Small INSERT (should work) ==='
INSERT INTO debug_wal_heap VALUES (1, 'test1', true, now());
INSERT INTO debug_wal_optimized VALUES (1, 'test1', true, now());

\echo '=== Phase 2: Batch INSERT (might trigger WAL issues) ==='
INSERT INTO debug_wal_heap 
SELECT i, 'text_' || i, (i % 2 = 0), '2025-01-01'::timestamp + (i || ' minutes')::interval
FROM generate_series(1, 100) i;

INSERT INTO debug_wal_optimized 
SELECT i, 'text_' || i, (i % 2 = 0), '2025-01-01'::timestamp + (i || ' minutes')::interval
FROM generate_series(1, 100) i;

\echo '=== Phase 3: SELECT operations (this is where crash occurred) ==='
SELECT COUNT(*) FROM debug_wal_heap WHERE id % 2 = 0;
\echo 'Heap SELECT completed'

SELECT COUNT(*) FROM debug_wal_optimized WHERE id % 2 = 0;
\echo 'Optimized SELECT completed'

\echo '=== Phase 4: Larger dataset (trigger more WAL activity) ==='
INSERT INTO debug_wal_optimized 
SELECT i, 'sample_text_' || (i % 50), (i % 3 = 0), '2025-01-01'::timestamp + (i || ' seconds')::interval
FROM generate_series(101, 1000) i;

SELECT COUNT(*) FROM debug_wal_optimized WHERE text_col LIKE 'sample%';
\echo 'Large dataset SELECT completed'

\echo '=== WAL Debug Test Completed Successfully ==='
