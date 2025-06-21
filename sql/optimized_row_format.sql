-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION optimized_row_format" to load this file. \quit

-- Create the access method
CREATE ACCESS METHOD optimized_row_format TYPE TABLE HANDLER optimized_row_format_tableam_handler;

-- Create the handler function
CREATE FUNCTION optimized_row_format_tableam_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- Register the access method
COMMENT ON ACCESS METHOD optimized_row_format IS 'optimized row format for tables';

-- Create the system catalog for optimized table metadata
CREATE TABLE pg_optimized_table_metadata (
    relid oid NOT NULL,
    fixed_col_count smallint NOT NULL,
    var_col_count smallint NOT NULL,
    fixed_col_offsets int2vector NOT NULL
);

-- Create a unique index on relid
CREATE UNIQUE INDEX pg_optimized_table_metadata_relid_index
    ON pg_optimized_table_metadata (relid);

-- Register the table as a system catalog
SELECT pg_catalog.pg_extension_config_dump('pg_optimized_table_metadata', '');

-- Add comment to the table
COMMENT ON TABLE pg_optimized_table_metadata IS 'Metadata for tables using optimized row format';

-- Add comments to columns
COMMENT ON COLUMN pg_optimized_table_metadata.relid IS 'OID of the relation using optimized row format';
COMMENT ON COLUMN pg_optimized_table_metadata.fixed_col_count IS 'Number of fixed-length columns in the relation';
COMMENT ON COLUMN pg_optimized_table_metadata.var_col_count IS 'Number of variable-length columns in the relation';
COMMENT ON COLUMN pg_optimized_table_metadata.fixed_col_offsets IS 'Array of offsets to fixed-length columns';

-- Create function to initialize metadata for a new table
CREATE FUNCTION init_optimized_table_metadata(relid oid)
RETURNS void
AS 'MODULE_PATHNAME', 'init_optimized_table_metadata'
LANGUAGE C STRICT;

-- Create function to update metadata when table structure changes
CREATE FUNCTION update_optimized_table_metadata(relid oid)
RETURNS void
AS 'MODULE_PATHNAME', 'update_optimized_table_metadata'
LANGUAGE C STRICT;

-- Create function to get metadata for a table
CREATE FUNCTION get_optimized_table_metadata(relid oid)
RETURNS pg_optimized_table_metadata
AS 'MODULE_PATHNAME', 'get_optimized_table_metadata'
LANGUAGE C STRICT;