-- Extension verification script
-- This script checks if the optimized_row_format extension is properly installed

\echo '=== Optimized Row Format Extension Verification ==='

-- Create the extension if it doesn't exist
\echo '=== Creating extension (if not exists) ==='
CREATE EXTENSION IF NOT EXISTS optimized_row_format;

-- Check if extension is available
\echo '=== Checking extension availability ==='
SELECT
    CASE
        WHEN EXISTS (
            SELECT 1 FROM pg_am WHERE amname = 'optimized_row_format'
        ) THEN '✅ Extension is available'
        ELSE '❌ Extension is NOT available'
    END as extension_status;

-- Check if we can create a table with the access method
\echo '=== Testing table creation and operations ==='
DO $$
BEGIN
    CREATE TABLE verify_test (
        id INTEGER,
        name TEXT
    ) USING optimized_row_format;

    RAISE NOTICE '✅ Successfully created table with optimized_row_format';

    -- Insert a test row
    INSERT INTO verify_test VALUES (1, 'test');
    RAISE NOTICE '✅ Successfully inserted data';

    -- Select the data
    PERFORM * FROM verify_test WHERE id = 1;
    RAISE NOTICE '✅ Successfully selected data';

    -- Test multiple rows
    INSERT INTO verify_test VALUES (2, 'test2'), (3, 'test3');
    PERFORM COUNT(*) FROM verify_test;
    RAISE NOTICE '✅ Successfully inserted and counted multiple rows';

    -- Cleanup
    DROP TABLE verify_test;
    RAISE NOTICE '✅ Successfully dropped table';

EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE '❌ Error: %', SQLERRM;
END $$;

-- Show available access methods
\echo ''
\echo '=== Available access methods ==='
SELECT amname, amtype FROM pg_am ORDER BY amname;

-- Show extension information if available
\echo ''
\echo '=== Extension information ==='
SELECT
    extname,
    extversion,
    extrelocatable
FROM pg_extension
WHERE extname = 'optimized_row_format';

-- Show extension files
\echo ''
\echo '=== Extension files ==='
SELECT
    proname,
    prosrc IS NOT NULL as has_source
FROM pg_proc
WHERE proname LIKE '%optimized%'
ORDER BY proname;

\echo ''
\echo '=== Verification Complete ==='