-- Simple UPDATE test to verify the fix works
CREATE EXTENSION IF NOT EXISTS optimized_row_format;

-- Create test table
DROP TABLE IF EXISTS update_test;
CREATE TABLE update_test (
    id INTEGER,
    name TEXT,
    value INTEGER
) USING optimized_row_format;

-- Insert test data
INSERT INTO update_test VALUES (1, 'Alice', 100), (2, 'Bob', 200);

-- Show initial data
SELECT * FROM update_test ORDER BY id;

-- Test UPDATE operation (this was crashing before)
UPDATE update_test SET value = 150 WHERE id = 1;

-- Verify the update worked
SELECT * FROM update_test ORDER BY id;

-- Test UPDATE with text column
UPDATE update_test SET name = 'Alice_Updated' WHERE id = 1;

-- Verify text update worked
SELECT * FROM update_test ORDER BY id;

-- Clean up
DROP TABLE update_test;
