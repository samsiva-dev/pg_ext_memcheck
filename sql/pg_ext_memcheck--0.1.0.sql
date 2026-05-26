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
    Table to store violation logs containing details about memory issues detected by the extension.
    check_type: type of violation (e.g., "context_leak", "wrong_ctx_alloc")
    severity: severity level of the violation (e.g., "ERROR", "WARNING", "INFO", "OK")
    detail: detailed message about the violation, including context names and sizes for context_leak, or source library for wrong_ctx_alloc
    source_lib: the shared library (extension) that was active when the violation was detected, for easier debugging and filtering of results.
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

    ext_memcheck.begin - Start a test session targeting an extension by context name pattern with optional configuration parameters
     - ext_context_pattern: a SQL LIKE pattern to filter context names for violation reporting (e.g., '%MyExtension%')
     - options: JSONB object for additional configuration (e.g., {"allowed_contexts": ["TopMemoryContext"]})
     - Returns: a status message
    ext_memcheck.end - End the current test session and trigger violation analysis
     - Returns: check_type TEXT, severity TEXT, detail TEXT, ts TIMESTAMPTZ
     - Resets the session state for the next test
    ext_memcheck.run_scenario - Run a predefined test scenario by name (e.g. 'growth_benchmark', 'tx_abort_loop', 'shmem_sentinel_probe', 'wrong_context_probe')
*/

CREATE OR REPLACE FUNCTION ext_memcheck.begin(
    ext_context_pattern TEXT DEFAULT '',
    options JSONB DEFAULT NULL
)
RETURNS TEXT
AS 'pg_ext_memcheck', 'memcheck_begin'
LANGUAGE C;

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

-- Utility function to clear the violation log table
CREATE OR REPLACE FUNCTION ext_memcheck.clear_violations()
RETURNS void
LANGUAGE SQL
AS $$
DELETE FROM ext_memcheck.violation_log;
$$;

-- Available Scenarios view for users to query and know what scenarios 
-- they can run with ext_memcheck.run_scenario()
CREATE VIEW ext_memcheck.scenarios AS
SELECT * FROM (
    VALUES
    ('growth_benchmark', 'Measures context size growth over N invocations'),
    ('tx_abort_loop', 'Runs N savepoint/rollback cycles to test abort-path cleanup'),
    ('shmem_sentinel_probe', 'Plants sentinel bytes around shmem allocations and verifies integrity after workload'),
    ('wrong_context_probe', 'Checks for allocations that land in long-lived contexts and reports violations')
) AS scenarios(name, description);


-- DSM tracking view
CREATE OR REPLACE FUNCTION ext_memcheck.dsm_tracking()
RETURNS TABLE(
    segid BIGINT,
    backend_pid INTEGER,
    attach_at TIMESTAMPTZ,
    size_bytes BIGINT,
    detached BOOLEAN
) AS 'pg_ext_memcheck', 'dsm_tracker_list_segments'
LANGUAGE C STRICT;

-- Tracking API for DSM segments
CREATE OR REPLACE FUNCTION ext_memcheck.track_dsm_handle(
    handle BIGINT
) RETURNS TEXT
AS 'pg_ext_memcheck', 'dsm_tracker_handle'
LANGUAGE C STRICT;

-- Utility functions to clear tracking state between tests
CREATE OR REPLACE FUNCTION ext_memcheck.clear_dsm_tracking()
RETURNS void
AS 'pg_ext_memcheck', 'clear_dsm_tracking'
LANGUAGE C STRICT;

-- API to register a user's extension shared memory probe callback
CREATE OR REPLACE FUNCTION ext_memcheck.register_shmem_probe(
    seg_name TEXT,
    allocated_size BIGINT
) RETURNS TEXT
AS 'pg_ext_memcheck', 'shmem_probe_register'
LANGUAGE C STRICT;

-- Check whether a previously registered sentinel byte is still intact.
-- Returns TRUE if the 0xDE sentinel at data_end is unchanged, FALSE otherwise.
-- Useful after register_shmem_probe() to confirm the sentinel was actually planted.
-- seg_name must match exactly the name used in ShmemInitStruct for the target segment.
CREATE OR REPLACE FUNCTION ext_memcheck.probe_check(
    seg_name TEXT
) RETURNS BOOLEAN
AS 'pg_ext_memcheck', 'shmem_probe_check'
LANGUAGE C STRICT;

-- Utility function to clear the shared memory registry
CREATE OR REPLACE FUNCTION ext_memcheck.clear_shmem_registry()
RETURNS void
AS 'pg_ext_memcheck', 'shmem_probe_clear_registry'
LANGUAGE C STRICT;

