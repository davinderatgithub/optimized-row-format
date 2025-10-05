-- Enable Debug Logging for SELECT Path Analysis
-- Run this first to enable detailed logging

\echo '=== Enabling Debug Logging ==='

-- Enable detailed logging
SET log_min_messages = debug1;
SET log_statement = 'all';
SET log_duration = on;
SET log_min_duration_statement = 0;
SET client_min_messages = debug1;

-- Enable function entry/exit logging (if compiled with debug)
SET log_executor_stats = on;
SET log_planner_stats = on;

-- Show current settings
SHOW log_min_messages;
SHOW log_statement;
SHOW log_duration;

\echo '=== Debug logging enabled ==='
\echo 'Now run your SELECT queries and check PostgreSQL logs'
\echo 'Log location is typically: /usr/local/pgsql/data/log/ or /var/log/postgresql/'
