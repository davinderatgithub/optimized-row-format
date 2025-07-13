#!/bin/bash

# Simple Many Columns Test Runner
# This script runs a simplified version of the many columns test with explicit transactions

set -e

# Default PostgreSQL installation
PSQL_CMD="psql"

# Check if custom PostgreSQL directory is provided
if [ $# -eq 1 ]; then
    CUSTOM_PG_DIR="$1"
    PSQL_CMD="$CUSTOM_PG_DIR/bin/psql"
    echo "Using custom PostgreSQL installation: $CUSTOM_PG_DIR"
else
    echo "Using system PostgreSQL installation"
    echo "To use a custom installation, run: $0 /path/to/postgres"
fi

# Check if psql exists
if ! command -v "$PSQL_CMD" &> /dev/null; then
    echo "Error: $PSQL_CMD not found"
    exit 1
fi

echo "=== Simple Many Columns Test ==="
echo "This test uses explicit transactions to avoid commit issues"
echo ""

# Database name
DB_NAME="many_cols_test_db"

# Try to create database, but don't fail if it already exists
echo "Setting up database..."
"$PSQL_CMD" -d postgres -c "CREATE DATABASE $DB_NAME;" 2>/dev/null || echo "Database $DB_NAME already exists"

# Run the simple test
echo "Running simple many columns test..."
"$PSQL_CMD" -d "$DB_NAME" -f simple_many_columns_test.sql

echo ""
echo "=== Test completed ==="
echo "If you see 0 rows but large table sizes, there may be an issue with the extension."
echo "Run the debug script to investigate further:"
echo "  $PSQL_CMD -d $DB_NAME -f debug_many_columns.sql"