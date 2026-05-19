-- -------------------------------------------------------------------------
-- pg_ext_memcheck
--
-- Copyright (c) 2026, Samba Siva Reddy
--
-- This software is released under the MIT License.
-- See LICENSE for details.
-- -------------------------------------------------------------------------

CREATE SCHEMA IF NOT EXISTS ext_memcheck;


/* 
    Table to store violation logs 
*/
CREATE TABLE IF NOT EXISTS ext_memcheck.violation_log (
    id SERIAL PRIMARY KEY,
    ts TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    backend_pid INTEGER NOT NULL,
    check_type TEXT NOT NULL,
    severity TEXT NOT NULL,
    detail TEXT NOT NULL,
    source_lib TEXT NOT NULL DEFAULT ''
);

/*
    Flush the in-memory violation ring buffer into the violation_log table.
    Returns the number of rows inserted.
    Usage: SELECT ext_memcheck.flush_violations();
*/
CREATE OR REPLACE FUNCTION ext_memcheck.flush_violations()
RETURNS INTEGER
AS 'pg_ext_memcheck', 'violation_log_flush'
LANGUAGE C STRICT;

/*
    SQL API for memcheck

    ext_memcheck.begin - Start a test session targeting an extension by context name pattern
    ext_memcheck.end - End the current test session and trigger violation analysis
     - Returns: check_type TEXT, severity TEXT, detail TEXT, ts TIMESTAMPTZ
    ext_memcheck.run_scenario - Run a predefined test scenario by name (e.g. 'context_reset_storm', 'tx_abort_loop', etc.)
*/

CREATE OR REPLACE FUNCTION ext_memcheck.begin(target_mode TEXT)
RETURNS TEXT
AS 'pg_ext_memcheck', 'memcheck_begin'
LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION ext_memcheck.end()
RETURNS TABLE(check_type TEXT, severity TEXT, detail TEXT, ts TIMESTAMPTZ, source_lib TEXT)
AS 'pg_ext_memcheck', 'memcheck_end'
LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION ext_memcheck.run_scenario(
    scenario_name TEXT,
    iterations INTEGER DEFAULT 100,
    workload TEXT DEFAULT 'SELECT 1'
)
RETURNS TEXT
AS 'pg_ext_memcheck', 'memcheck_run_scenario'
LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION ext_memcheck.clear_violations()
RETURNS void
LANGUAGE SQL
AS $$
DELETE FROM ext_memcheck.violation_log;
$$;

-- Scenarios view
CREATE VIEW ext_memcheck.scenarios AS
SELECT * FROM (
    VALUES
    ('growth_benchmark', 'Measures context size growth over N invocations'),
    ('tx_abort_loop', 'Runs N savepoint/rollback cycles to test abort-path cleanup')
) AS scenarios(name, description);