#!/bin/bash

# Performance test runner for optimized_row_format extension
# Usage: ./run_performance_test.sh [--update-expected]

set -e

# Configuration
POSTGRES_BIN="../../../../build/bin"
TEST_DB="postgres"
PERFORMANCE_SQL="sql/performance.sql"
RESULTS_DIR="results"
EXPECTED_DIR="expected"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=== Optimized Row Format Performance Test Runner ==="

# Check if PostgreSQL is running
if ! $POSTGRES_BIN/pg_isready -q; then
    echo -e "${RED}Error: PostgreSQL server is not running${NC}"
    echo "Please start the server with: $POSTGRES_BIN/pg_ctl -D data -l logfile start"
    exit 1
fi

# Create results directory if it doesn't exist
mkdir -p "$RESULTS_DIR"

echo "Running performance tests..."

# Run the performance test
if $POSTGRES_BIN/psql -d "$TEST_DB" -f "$PERFORMANCE_SQL" > "$RESULTS_DIR/performance.out" 2>&1; then
    echo -e "${GREEN}✅ Performance test completed successfully${NC}"
    
    # Show key performance metrics
    echo ""
    echo "=== Performance Summary ==="
    grep -E "(INSERT Performance|SELECT Performance|Speedup:|Storage)" "$RESULTS_DIR/performance.out" | \
        grep -E "(NOTICE|test_case)" | \
        sed 's/psql:sql\/performance.sql:[0-9]*: NOTICE:  //'
    
    # Update expected file if requested
    if [[ "$1" == "--update-expected" ]]; then
        cp "$RESULTS_DIR/performance.out" "$EXPECTED_DIR/performance.out"
        echo -e "${YELLOW}📝 Updated expected results file${NC}"
    fi
    
    echo ""
    echo "Results saved to: $RESULTS_DIR/performance.out"
    echo "To update expected results: $0 --update-expected"
    
else
    echo -e "${RED}❌ Performance test failed${NC}"
    echo "Check the output in: $RESULTS_DIR/performance.out"
    exit 1
fi
