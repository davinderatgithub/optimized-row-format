#!/bin/bash

# Run a subset of PostgreSQL regression tests with optimized_row_format as default
# This focuses on the most critical tests that would expose contract violations

set -e

POSTGRES_DIR="/Users/davindersingh/personal/postgres"
SOURCE_DIR="$POSTGRES_DIR/source"
BUILD_DIR="$POSTGRES_DIR/build"

echo "=== Running PostgreSQL regression tests with optimized_row_format ==="

# Change to source directory
cd "$SOURCE_DIR"

# Critical tests that would expose our contract violations
CRITICAL_TESTS=(
    "create_table"      # Basic table creation
    "insert"           # INSERT operations
    "select"           # SELECT operations - most likely to crash
    "select_distinct"  # More complex SELECT patterns
    "select_having"    # SELECT with complex expressions
    "select_implicit"  # Implicit column access
    "expressions"      # Expression evaluation (uses execExprInterp.c)
    "join"             # JOIN operations with multiple column access
    "aggregates"       # Aggregate functions
    "subselect"        # Subqueries
    "union"            # UNION operations
    "case"             # CASE expressions
    "arrays"           # Array operations
    "rowtypes"         # Row type operations
    "returning"        # RETURNING clauses
)

# Function to run a single test
run_test() {
    local test_name=$1
    echo ""
    echo "=== Running test: $test_name ==="
    
    if make check TESTS="$test_name" > "test_${test_name}.log" 2>&1; then
        echo "✅ PASSED: $test_name"
        return 0
    else
        echo "❌ FAILED: $test_name"
        echo "Last 20 lines of log:"
        tail -20 "test_${test_name}.log"
        return 1
    fi
}

# Create results directory
mkdir -p regression_results
cd regression_results

# Run each critical test
PASSED=0
FAILED=0
FAILED_TESTS=()

for test in "${CRITICAL_TESTS[@]}"; do
    if run_test "$test"; then
        ((PASSED++))
    else
        ((FAILED++))
        FAILED_TESTS+=("$test")
    fi
done

# Summary
echo ""
echo "=== REGRESSION TEST SUMMARY ==="
echo "Total tests: $((PASSED + FAILED))"
echo "Passed: $PASSED"
echo "Failed: $FAILED"

if [ $FAILED -gt 0 ]; then
    echo ""
    echo "Failed tests:"
    for test in "${FAILED_TESTS[@]}"; do
        echo "  - $test"
    done
    echo ""
    echo "This indicates potential PostgreSQL contract violations!"
    echo "Check the logs in regression_results/ for details."
    exit 1
else
    echo ""
    echo "🎉 All critical tests passed!"
    echo "The optimized_row_format appears to be compatible with PostgreSQL's expectations."
fi
