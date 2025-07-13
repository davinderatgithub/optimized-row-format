#!/bin/bash

# Many Columns Test Runner for Optimized Row Format
# This script runs the focused test on tables with 100 columns

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
DB_NAME="many_cols_test_db"
RESULTS_FILE="many_columns_results_$(date +%Y%m%d_%H%M%S).txt"
TEST_SQL="many_columns_test.sql"
CREATED_DB=false

# PostgreSQL paths - will be set based on input
PG_BINDIR=""
PG_PSQL=""
PG_CREATEDB=""
PG_DROPDB=""
PG_ISREADY=""

echo -e "${BLUE}=== Many Columns Performance Test Runner ===${NC}"
echo "Results will be saved to: $RESULTS_FILE"
echo ""

# Function to set PostgreSQL paths
set_pg_paths() {
    local pg_dir="$1"

    if [ -z "$pg_dir" ]; then
        # Use system PostgreSQL if no directory specified
        PG_BINDIR=""
        PG_PSQL="psql"
        PG_CREATEDB="createdb"
        PG_DROPDB="dropdb"
        PG_ISREADY="pg_isready"
        echo -e "${YELLOW}Using system PostgreSQL installation${NC}"
    else
        # Use custom PostgreSQL installation
        if [ ! -d "$pg_dir" ]; then
            echo -e "${RED}Error: PostgreSQL directory does not exist: $pg_dir${NC}"
            exit 1
        fi

        PG_BINDIR="$pg_dir/bin"
        PG_PSQL="$PG_BINDIR/psql"
        PG_CREATEDB="$PG_BINDIR/createdb"
        PG_DROPDB="$PG_BINDIR/dropdb"
        PG_ISREADY="$PG_BINDIR/pg_isready"

        # Check if binaries exist
        for binary in "$PG_PSQL" "$PG_CREATEDB" "$PG_DROPDB" "$PG_ISREADY"; do
            if [ ! -x "$binary" ]; then
                echo -e "${RED}Error: PostgreSQL binary not found: $binary${NC}"
                exit 1
            fi
        done

        echo -e "${GREEN}Using custom PostgreSQL installation: $pg_dir${NC}"
        echo -e "${GREEN}PostgreSQL binaries: $PG_BINDIR${NC}"
    fi
}

# Function to check if PostgreSQL is running
check_postgres() {
    if ! "$PG_ISREADY" -q; then
        echo -e "${RED}Error: PostgreSQL is not running${NC}"
        echo "Please start PostgreSQL before running tests"
        echo "You can start it with: $PG_BINDIR/pg_ctl start -D /path/to/data"
        exit 1
    fi
}

# Function to create test database
create_test_db() {
    echo -e "${YELLOW}Creating test database: $DB_NAME${NC}"

    # Try to create database, but handle custom PostgreSQL builds
    if "$PG_CREATEDB" "$DB_NAME" 2>/dev/null; then
        echo -e "${GREEN}Database created successfully${NC}"
        CREATED_DB=true
    else
        echo -e "${YELLOW}Could not create database, trying to use existing database${NC}"

        # Try to connect to existing database
        if "$PG_PSQL" -d "$DB_NAME" -c "SELECT 1;" >/dev/null 2>&1; then
            echo -e "${GREEN}Using existing database: $DB_NAME${NC}"
        else
            # Try to use postgres database
            DB_NAME="postgres"
            if "$PG_PSQL" -d "$DB_NAME" -c "SELECT 1;" >/dev/null 2>&1; then
                echo -e "${GREEN}Using default database: $DB_NAME${NC}"
            else
                echo -e "${RED}Error: Cannot connect to any database${NC}"
                echo "Please ensure PostgreSQL is running and you have access to a database"
                exit 1
            fi
        fi
    fi
}

# Function to run test
run_test() {
    echo -e "${YELLOW}Running many columns performance test...${NC}"
    echo "This test will:"
    echo "  - Create tables with 100 columns (50 fixed + 50 variable)"
    echo "  - Insert 10,000 rows"
    echo "  - Test single-column selection performance"
    echo "  - Compare storage sizes"
    echo "  - Test full table scan performance"
    echo ""
    echo "This may take several minutes..."
    echo ""

    # Run the test SQL
    "$PG_PSQL" -d "$DB_NAME" -f "$TEST_SQL" 2>&1 | tee "$RESULTS_FILE"

    echo ""
    echo -e "${GREEN}Test completed!${NC}"
    echo "Results saved to: $RESULTS_FILE"
}

# Function to analyze results
analyze_results() {
    echo -e "${BLUE}=== Test Analysis ===${NC}"

    if [ -f "$RESULTS_FILE" ]; then
        echo ""
        echo -e "${YELLOW}Performance Summary:${NC}"
        echo "========================"

        # Extract performance metrics
        echo -e "${GREEN}INSERT Performance:${NC}"
        grep -A 2 "INSERT.*10,000 rows" "$RESULTS_FILE" | grep -E "(Heap|Optimized)" || echo "No INSERT results found"

        echo ""
        echo -e "${GREEN}Fixed-Length Column SELECT Performance:${NC}"
        grep -A 3 "SELECT col[0-9]* (fixed-length)" "$RESULTS_FILE" | grep -E "(Heap|Optimized|Speedup)" || echo "No fixed-length SELECT results found"

        echo ""
        echo -e "${GREEN}Variable-Length Column SELECT Performance:${NC}"
        grep -A 3 "SELECT col[0-9]* (variable-length)" "$RESULTS_FILE" | grep -E "(Heap|Optimized|Speedup)" || echo "No variable-length SELECT results found"

        echo ""
        echo -e "${GREEN}Storage Size Comparison:${NC}"
        grep -A 5 "Storage Size Comparison" "$RESULTS_FILE" || echo "No storage results found"

        echo ""
        echo -e "${GREEN}Full Table Scan Performance:${NC}"
        grep -A 3 "Full Table Scan Performance" "$RESULTS_FILE" | grep -E "(Heap|Optimized|Speedup)" || echo "No full scan results found"

    else
        echo -e "${RED}Results file not found: $RESULTS_FILE${NC}"
    fi
}

# Function to cleanup
cleanup() {
    echo -e "${YELLOW}Cleaning up...${NC}"

    # Only drop database if we created it
    if [ "$CREATED_DB" = true ]; then
        "$PG_DROPDB" --if-exists "$DB_NAME" 2>/dev/null || true
        echo -e "${GREEN}Test database dropped${NC}"
    else
        echo -e "${YELLOW}Database was not created by this script, leaving it as is${NC}"
    fi

    echo -e "${GREEN}Cleanup completed${NC}"
}

# Main execution
main() {
    echo -e "${BLUE}Starting many columns test...${NC}"

    # Set PostgreSQL paths
    set_pg_paths "$1"

    # Check prerequisites
    check_postgres

    # Create test database
    create_test_db

    # Run test
    run_test

    # Analyze results
    analyze_results

    # Ask if user wants to cleanup
    echo ""
    read -p "Do you want to clean up the test database? (y/n): " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        cleanup
    else
        echo -e "${YELLOW}Test database '$DB_NAME' left for manual inspection${NC}"
    fi

    echo ""
    echo -e "${GREEN}Many columns test completed!${NC}"
}

# Help function
show_help() {
    echo "Usage: $0 [POSTGRES_DIR]"
    echo ""
    echo "Arguments:"
    echo "  POSTGRES_DIR    Path to custom PostgreSQL installation directory"
    echo "                   (e.g., /path/to/postgres/install)"
    echo "                   If not specified, uses system PostgreSQL"
    echo ""
    echo "Examples:"
    echo "  $0                                    # Use system PostgreSQL"
    echo "  $0 /path/to/postgres/install          # Use custom PostgreSQL"
    echo ""
    echo "This test focuses on:"
    echo "  - Tables with 100 columns (50 fixed + 50 variable)"
    echo "  - No primary keys"
    echo "  - Single-column selection performance"
    echo "  - INSERT performance with 10,000 rows"
    echo "  - Storage size comparison"
    echo ""
    echo "Prerequisites:"
    echo "  - PostgreSQL must be running"
    echo "  - Extension must be built and installed"
    echo "  - User must have database creation privileges"
}

# Parse command line arguments
case "$1" in
    --help)
        show_help
        exit 0
        ;;
    "")
        # No arguments - use system PostgreSQL
        main ""
        ;;
    *)
        # First argument is PostgreSQL directory
        main "$1"
        ;;
esac