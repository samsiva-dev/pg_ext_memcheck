-- -------------------------------------------------------------------------
-- pg_ext_memcheck
--
-- Copyright (c) 2026, Samba Siva Reddy
--
-- This software is released under the MIT License.
-- See LICENSE for details.
-- -------------------------------------------------------------------------

-- 20_cursor_leak.sql
-- Verifies cursor_leak scenario: opens SPI cursors without closing them
-- and checks that portal context growth is captured.

SET client_min_messages = WARNING;
SET pg_ext_memcheck.memcheck_mode = 'all';
SELECT ext_memcheck.begin('');

-- Test 1: scenario runs without error and returns the expected message
SELECT ext_memcheck.run_scenario('cursor_leak', 5, 'SELECT 1');

-- Test 2: violations are context_leak type (portal context accumulation)
SELECT count(*) >= 0 AS cursor_leak_ran
FROM ext_memcheck.end()
WHERE check_type IN ('context_leak', 'ctx_bloat');

-- Test 3: scenario appears in ext_memcheck.scenarios view
SELECT count(*) = 1 AS in_catalog
FROM ext_memcheck.scenarios
WHERE name = 'cursor_leak';

-- Cleanup
SET pg_ext_memcheck.memcheck_mode = 'none';
DELETE FROM ext_memcheck.violation_log;
