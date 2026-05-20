-- -------------------------------------------------------------------------
-- pg_ext_memcheck
--
-- Copyright (c) 2026, Samba Siva Reddy
--
-- This software is released under the MIT License.
-- See LICENSE for details.
-- -------------------------------------------------------------------------

-- 09_flush_clears_buffer.sql
-- After flush_violations(), the ring buffer must be cleared so that a
-- subsequent flush returns 0 and end() returns 0 rows.

SET client_min_messages = WARNING;
SET pg_ext_memcheck.memcheck_mode = 'all';
SELECT ext_memcheck.flush_violations();
DELETE FROM ext_memcheck.violation_log;

-- Generate some violations
SELECT ext_memcheck.run_scenario('growth_benchmark', 5);

-- First flush inserts rows and clears the buffer
SELECT ext_memcheck.flush_violations() >= 0 AS first_flush_ok;

-- Second flush of the now-empty buffer must return 0
SELECT ext_memcheck.flush_violations() AS second_flush_should_be_zero;

-- end() on the cleared buffer must return 0 rows
SELECT ext_memcheck.begin('none');
SELECT count(*) AS viol_after_flush FROM ext_memcheck.end();

-- Cleanup
DELETE FROM ext_memcheck.violation_log;
SET pg_ext_memcheck.memcheck_mode = 'none';
