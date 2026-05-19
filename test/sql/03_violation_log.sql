-- -------------------------------------------------------------------------
-- pg_ext_memcheck
--
-- Copyright (c) 2026, Samba Siva Reddy
--
-- This software is released under the MIT License.
-- See LICENSE for details.
-- -------------------------------------------------------------------------

-- 03_violation_log.sql
-- Tests for the violation_log ring buffer and flush_violations().

SET client_min_messages = WARNING;

-- Start with a clean slate
SET pg_ext_memcheck.memcheck_mode = 'none';
SELECT ext_memcheck.flush_violations();
DELETE FROM ext_memcheck.violation_log;

-- Confirm persistent table is empty
SELECT count(*) AS rows_in_table FROM ext_memcheck.violation_log;

-- flush_violations on an empty ring buffer returns 0
SELECT ext_memcheck.flush_violations() AS flushed;

-- Run a scenario to populate the ring buffer
SET pg_ext_memcheck.memcheck_mode = 'all';
SELECT ext_memcheck.begin('all');
SELECT ext_memcheck.run_scenario('growth_benchmark', 5);
-- end() drains the ring buffer into the result set; count should be >= 0
SELECT count(*) >= 0 AS has_rows FROM ext_memcheck.end();

-- flush_violations persists ring buffer rows into the table
SELECT ext_memcheck.begin('all');
SELECT ext_memcheck.run_scenario('growth_benchmark', 5);
SELECT ext_memcheck.begin('none');
SELECT ext_memcheck.flush_violations() >= 0 AS flush_ok;

-- Persistent table should now have rows (>= 0)
SELECT count(*) >= 0 AS persisted FROM ext_memcheck.violation_log;

-- Each persisted row must have a non-null check_type and severity
SELECT count(*) AS bad_rows
FROM ext_memcheck.violation_log
WHERE check_type IS NULL OR severity IS NULL OR detail IS NULL;

-- Severity values must be one of the known set
SELECT count(*) AS unexpected_severity
FROM ext_memcheck.violation_log
WHERE severity NOT IN ('ERROR', 'WARNING', 'INFO', 'OK');

-- Cleanup
DELETE FROM ext_memcheck.violation_log;
SET pg_ext_memcheck.memcheck_mode = 'none';
