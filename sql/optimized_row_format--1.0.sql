-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION optimized_row_format" to load this file. \quit

-- Create the handler function
CREATE FUNCTION optimized_row_format_tableam_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- Create the access method
CREATE ACCESS METHOD optimized_row_format TYPE TABLE HANDLER optimized_row_format_tableam_handler;

-- Register the access method
COMMENT ON ACCESS METHOD optimized_row_format IS 'optimized row format for tables';