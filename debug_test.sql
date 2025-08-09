-- Debug test to isolate the smoke test issue
-- First, test minimal functionality

\echo 'Starting debug test...'

CREATE EXTENSION IF NOT EXISTS optimized_row_format;

\echo 'Extension created successfully'

-- Create a simple table
CREATE TABLE debug_test (
    id int,
    name text
) USING optimized_row_format;

\echo 'Table created successfully'

-- Insert one row
INSERT INTO debug_test VALUES (1, 'test');

\echo 'First insert completed'

-- Try to select
SELECT * FROM debug_test;

\echo 'First select completed'

-- Insert another row  
INSERT INTO debug_test VALUES (2, 'test2');

\echo 'Second insert completed'

-- Try to select both
SELECT * FROM debug_test ORDER BY id;

\echo 'Final select completed'

-- Cleanup
DROP TABLE debug_test;

\echo 'Debug test completed successfully'
