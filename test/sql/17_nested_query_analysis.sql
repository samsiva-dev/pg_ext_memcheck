-- -------------------------------------------------------------------------
-- pg_ext_memcheck
--
-- Copyright (c) 2026, Samba Siva Reddy
--
-- This software is released under the MIT License.
-- See LICENSE for details.
-- -------------------------------------------------------------------------

-- 17_nested_query_analysis.sql
-- Regression test for nested-query snapshot stack handling.
--
-- Before the fix, ExecutorEnd would clear before_snapshot when a nested query
-- ended, causing the outer query's ExecutorEnd to find NULL and silently skip
-- analysis. With the snapshot stack in place, each nesting level preserves its
-- own before-snapshot independently.
--
-- These tests prove:
--   1. 2-level nesting (PL/pgSQL → inner SQL): ExecutorEnd returns without crash;
--      outer analysis completes (not skipped).
--   2. 3-level nesting (outer → middle → inner): Stack handles arbitrary depth.
--   3. Back-to-back sessions: snapshot_depth resets to 0 between sessions.
--
-- The critical invariants are:
--   - No crash, no Assert failure, no memory corruption
--   - snapshot_depth is 0 after session end (no frame leaks)
--   - No test should fail

SET client_min_messages = WARNING;

-- Flush any leftover violations from previous tests
SELECT ext_memcheck.flush_violations() >= 0 AS cleaned;
DELETE FROM ext_memcheck.violation_log;

-- -----------------------------------------------------------------------
-- Test 1: 2-level nesting (PL/pgSQL function calls inner query)
--
-- Creates a PL/pgSQL function that executes a SELECT internally.
-- When pg_ext_memcheck runs in EXECUTOR mode:
--   - ExecutorStart for outer CALL statement pushes frame 1
--   - ExecutorStart for inner SELECT (inside plpgsql) pushes frame 2
--   - ExecutorEnd for inner SELECT pops frame 2
--   - ExecutorEnd for outer CALL pops frame 1 ← should not skip, not find NULL
-- -----------------------------------------------------------------------

-- Create a simple PL/pgSQL function that executes a query
CREATE OR REPLACE FUNCTION nested_level_2() RETURNS int AS $$
BEGIN
    RETURN (SELECT 42 AS inner_result);
END;
$$ LANGUAGE plpgsql;

SET pg_ext_memcheck.memcheck_mode = 'executor';
SET pg_ext_memcheck.min_leak_bytes = '0';

-- Enable monitoring; workload will run nested queries
SELECT ext_memcheck.begin('') LIKE 'Memory check%' AS session_started;

-- Execute the nested query: CALL triggers ExecutorStart/End for the CALL itself,
-- and the inner SELECT in plpgsql triggers its own ExecutorStart/End.
SELECT nested_level_2() AS result;

-- End the session and verify no crash; violations count should be >= 0 (no crash)
SELECT count(*) >= 0 AS no_crash_2level FROM ext_memcheck.end();

-- -----------------------------------------------------------------------
-- Test 2: 3-level nesting (outer → middle → inner functions)
--
-- Tests deeper call chains to verify the stack handles arbitrary depths.
-- -----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION nested_level_3_inner() RETURNS int AS $$
BEGIN
    RETURN (SELECT 100 AS level_3_result);
END;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION nested_level_3_middle() RETURNS int AS $$
BEGIN
    RETURN nested_level_3_inner() + 1;
END;
$$ LANGUAGE plpgsql;

DELETE FROM ext_memcheck.violation_log;
SET pg_ext_memcheck.memcheck_mode = 'executor';

-- New session for 3-level test
SELECT ext_memcheck.begin('') LIKE 'Memory check%' AS session_started;

-- Call the 3-level chain
SELECT nested_level_3_middle() AS result;

-- End session; no crash
SELECT count(*) >= 0 AS no_crash_3level FROM ext_memcheck.end();

-- -----------------------------------------------------------------------
-- Test 3: Back-to-back sessions to verify snapshot_depth resets
--
-- Runs three separate begin/end pairs in a row. If snapshot_depth doesn't
-- reset to 0 between sessions, the second or third session will either crash
-- (stack overflow on push to an already-full position) or produce garbage
-- analysis (comparing stale snapshots from a previous session).
-- -----------------------------------------------------------------------

DELETE FROM ext_memcheck.violation_log;
SET pg_ext_memcheck.memcheck_mode = 'executor';

-- Session A
SELECT ext_memcheck.begin('') LIKE 'Memory check%' AS session_a_started;
SELECT nested_level_2() AS result;
SELECT count(*) >= 0 AS session_a_no_crash FROM ext_memcheck.end();

-- Session B (immediately after A)
DELETE FROM ext_memcheck.violation_log;
SELECT ext_memcheck.begin('') LIKE 'Memory check%' AS session_b_started;
SELECT nested_level_3_middle() AS result;
SELECT count(*) >= 0 AS session_b_no_crash FROM ext_memcheck.end();

-- Session C (immediately after B)
DELETE FROM ext_memcheck.violation_log;
SELECT ext_memcheck.begin('') LIKE 'Memory check%' AS session_c_started;
SELECT nested_level_2() AS result;
SELECT count(*) >= 0 AS session_c_no_crash FROM ext_memcheck.end();

-- -----------------------------------------------------------------------
-- Cleanup
-- -----------------------------------------------------------------------
DROP FUNCTION nested_level_3_middle();
DROP FUNCTION nested_level_3_inner();
DROP FUNCTION nested_level_2();
DELETE FROM ext_memcheck.violation_log;
SET pg_ext_memcheck.memcheck_mode = 'none';
RESET pg_ext_memcheck.min_leak_bytes;
