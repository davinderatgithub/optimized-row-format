# Development Environment Setup

This document contains critical information about the custom development environment for the `optimized_row_format` extension. All agents performing build, installation, or testing tasks **must** adhere to these instructions.

## Project Repository Structure

**IMPORTANT: Main Extension Directory**
- **Primary Repository**: `contrib/optimized_row_format/` 
- **All extension-related work** should be committed to this directory
- **Git commits** should focus on changes within this directory structure
- **Development workflow** centers around this extension as the main deliverable

### Directory Structure:
```
contrib/optimized_row_format/          ← MAIN EXTENSION DIRECTORY
├── optimized_row_format.c             ← Core implementation 
├── optimized_row_format.h             ← Header definitions
├── optimized_row_format.control       ← Extension control file
├── Makefile                           ← Build configuration
├── sql/                              ← SQL installation scripts
├── test/                             ← Test suite
├── docs/                             ← Technical documentation
├── CURRENT_STATUS.md                 ← Current development status
├── development_environment.md        ← This file
└── recommended_next_steps.md         ← Action plans
```

**Git Workflow:**
- Focus commits on `contrib/optimized_row_format/` changes
- Use meaningful commit messages describing extension improvements
- Track progress through documentation updates in this directory

## Custom PostgreSQL Installation

The PostgreSQL instance used for this development is installed in a custom location.

-   **Installation Path:** `/Users/davindersingh/personal/postgres/build`
-   **Binaries Path:** `/Users/davindersingh/personal/postgres/build/bin`

When running any PostgreSQL command-line tools (`psql`, `pg_config`, `createdb`, etc.), you **must** use the full path to the binaries in this directory.

**Example:**
```bash
# Correct
/Users/davindersingh/personal/postgres/build/bin/psql -d my_database

# Incorrect
psql -d my_database
```

The test scripts (e.g., `run_benchmark.sh`, `run_many_columns_test.sh`) accept this path as an argument.

**Example:**
```bash
./run_many_columns_test.sh /Users/davindersingh/personal/postgres/build
```

## Build and Restart Procedure

After making changes to the C code and rebuilding the extension with `make && make install`, the PostgreSQL server **must be restarted** for the changes to take effect.

1.  **Stop the Server:**
    ```bash
    /Users/davindersingh/personal/postgres/build/bin/pg_ctl stop -D /Users/davindersingh/personal/postgres/build/bin/data -m fast
    ```
    *(Note: The data directory `-D` path above is an example. Please verify the correct path for your setup.)*

2.  **Start the Server:**
    ```bash
    /Users/davindersingh/personal/postgres/build/bin/pg_ctl start -D /Users/davindersingh/personal/postgres/build/bin/data
    ```

**Failure to restart the server after a rebuild is the most common cause of tests running against stale code.** Always include this restart step in your workflow before running any tests. 