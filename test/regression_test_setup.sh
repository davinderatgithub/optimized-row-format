#!/bin/bash

# PostgreSQL Regression Test Setup with Optimized Row Format as Default
# This script sets up PostgreSQL to use optimized_row_format as the default table access method

set -e

POSTGRES_DIR="/Users/davindersingh/personal/postgres"
BUILD_DIR="$POSTGRES_DIR/build"
DATA_DIR="$BUILD_DIR/data"

echo "=== Setting up PostgreSQL regression tests with optimized_row_format as default ==="

# 1. Stop PostgreSQL if running
echo "Stopping PostgreSQL..."
$BUILD_DIR/bin/pg_ctl -D $DATA_DIR stop -m fast || true

# 2. Build and install the extension
echo "Building optimized_row_format extension..."
cd $POSTGRES_DIR/source/contrib/optimized_row_format
make clean && make && make install

# 3. Modify postgresql.conf to set default table access method
echo "Configuring default_table_access_method..."
CONF_FILE="$DATA_DIR/postgresql.conf"

# Remove any existing default_table_access_method setting
sed -i.bak '/^default_table_access_method/d' "$CONF_FILE" || true
sed -i.bak '/^#default_table_access_method/d' "$CONF_FILE" || true

# Add our setting
echo "" >> "$CONF_FILE"
echo "# Optimized Row Format Testing" >> "$CONF_FILE"
echo "default_table_access_method = 'optimized_row_format'" >> "$CONF_FILE"

# Also add to shared_preload_libraries if not already there
if ! grep -q "optimized_row_format" "$CONF_FILE"; then
    echo "shared_preload_libraries = 'optimized_row_format'" >> "$CONF_FILE"
fi

# 4. Start PostgreSQL
echo "Starting PostgreSQL..."
$BUILD_DIR/bin/pg_ctl -D $DATA_DIR -l $BUILD_DIR/logfile start

# Wait for startup
sleep 3

# 5. Create the extension in template databases
echo "Creating extension in template databases..."
$BUILD_DIR/bin/psql -d template1 -c "CREATE EXTENSION IF NOT EXISTS optimized_row_format;"
$BUILD_DIR/bin/psql -d postgres -c "CREATE EXTENSION IF NOT EXISTS optimized_row_format;"

# 6. Verify the setup
echo "Verifying setup..."
$BUILD_DIR/bin/psql -d postgres -c "SHOW default_table_access_method;"
$BUILD_DIR/bin/psql -d postgres -c "SELECT amname FROM pg_am WHERE amname = 'optimized_row_format';"

# 7. Test table creation
echo "Testing default table creation..."
$BUILD_DIR/bin/psql -d postgres << 'EOF'
DROP TABLE IF EXISTS test_default;
CREATE TABLE test_default (id int, name text);
SELECT relname, (SELECT amname FROM pg_am WHERE oid = relam) as access_method 
FROM pg_class WHERE relname = 'test_default';
DROP TABLE test_default;
EOF

echo "=== Setup complete! ==="
echo "PostgreSQL is now configured to use optimized_row_format as default."
echo "You can now run regression tests with:"
echo "  cd $POSTGRES_DIR/source"
echo "  make check"
echo ""
echo "To restore heap as default:"
echo "  sed -i.bak \"s/default_table_access_method = 'optimized_row_format'/default_table_access_method = 'heap'/\" $CONF_FILE"
echo "  $BUILD_DIR/bin/pg_ctl -D $DATA_DIR restart"
