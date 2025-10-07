# GitHub Actions Workflows

This directory contains CI/CD workflows for the optimized_row_format PostgreSQL extension.

## Available Workflows

### 1. CI (`ci.yml`)
**Triggers**: Push to main/master/develop branches, Pull Requests, Manual dispatch

**Jobs**:
- **test**: Runs on multiple PostgreSQL versions (14, 15, 16, 17) and OS (Ubuntu, macOS)
  - Builds the extension
  - Runs smoke tests
  - Runs correctness tests
  - Runs functionality tests
  - Runs performance tests
  - Runs smart extraction tests
  - Uploads test results as artifacts

- **performance-benchmark**: Runs comprehensive benchmarks
  - 30-column wide table tests
  - 600-column extreme width tests (with timeout protection)
  - Uploads benchmark results

- **lint**: Code quality checks
  - Checks for trailing whitespace
  - Checks for tabs in SQL files
  - Lists TODO/FIXME comments
  - Verifies documentation exists

### 2. Performance Tracking (`performance-tracking.yml`)
**Triggers**: Daily at 2 AM UTC, Manual dispatch

**Purpose**: Track performance metrics over time to detect regressions

**Jobs**:
- Runs full performance test suite
- Generates performance report with key metrics
- Uploads report as artifact (90-day retention)
- Fails if INSERT performance drops below 1.0x (regression detection)

## How to Use

### Running Workflows Manually

1. Go to your repository on GitHub
2. Click on "Actions" tab
3. Select the workflow you want to run
4. Click "Run workflow" button
5. Choose the branch and click "Run workflow"

### Viewing Results

1. Go to "Actions" tab
2. Click on a workflow run
3. View job logs and download artifacts

### Artifacts

- **test-results-pg{version}-{os}**: Test output files
- **benchmark-results**: Performance benchmark outputs
- **performance-report-{sha}**: Daily performance tracking reports

## Local Testing

Before pushing, you can test locally:

```bash
# Build and test
make clean && make && make install
psql -d postgres -f test/sql/smoke.sql
psql -d postgres -f test/sql/correctness.sql
psql -d postgres -f test/sql/performance.sql
```

## Customization

### Adding New Tests

Edit `ci.yml` and add a new step:

```yaml
- name: Run my new test
  run: |
    psql -U postgres -d postgres -f test/sql/my_new_test.sql
```

### Testing Different PostgreSQL Versions

Edit the `matrix.pg_version` array in `ci.yml`:

```yaml
matrix:
  pg_version: [14, 15, 16, 17, 18]  # Add new versions
```

### Adjusting Performance Thresholds

Edit `performance-tracking.yml` to change regression detection:

```yaml
if (( $(echo "$INSERT_RATIO < 1.2" | bc -l) )); then  # Change threshold
```

## Troubleshooting

### Workflow Fails on PostgreSQL Installation

- Check if the PostgreSQL version is available in the runner's package repository
- Ubuntu runners may need `postgresql-server-dev-{version}` package
- macOS runners use Homebrew and may have different version availability

### Tests Timeout

- Adjust timeout in `ci.yml`:
  ```yaml
  timeout-minutes: 30  # Increase if needed
  ```

### Artifacts Not Uploading

- Check file paths exist
- Verify retention-days is within GitHub limits (max 90 days)
- Ensure `actions/upload-artifact@v4` is compatible with your GitHub plan

## Best Practices

1. **Always run tests locally first** before pushing
2. **Keep workflows fast** - split long-running tests into separate jobs
3. **Use matrix strategy** for testing multiple configurations
4. **Upload artifacts** for debugging failed runs
5. **Set appropriate timeouts** to prevent hung tests from blocking CI
6. **Monitor performance trends** using the tracking workflow
