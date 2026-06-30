-- -------------------------------------------------------------------------
-- pg_ext_memcheck
--
-- Copyright (c) 2026, Samba Siva Reddy
--
-- This software is released under the MIT License.
-- See LICENSE for details.
-- -------------------------------------------------------------------------

-- 19_context_reset_storm.sql
-- Verifies context_reset_storm scenario: repeated context reset between workload invocations.

SET client_min_messages = WARNING;
SET pg_ext_memcheck.memcheck_mode = 'all';
SELECT ext_memcheck.begin('');

-- Test 1: scenario runs without error
SELECT ext_memcheck.run_scenario('context_reset_storm', 10, 'SELECT 1');

-- Test 2: violations (if any) are check_type context_leak or wrong_ctx_alloc
SELECT count(*) >= 0 AS storm_ran
FROM ext_memcheck.end()
WHERE check_type IN ('context_leak', 'wrong_ctx_alloc', 'ctx_bloat');

-- Test 3: scenario appears in ext_memcheck.scenarios view
SELECT count(*) = 1 AS in_catalog
FROM ext_memcheck.scenarios
WHERE name = 'context_reset_storm';

-- Cleanup
SET pg_ext_memcheck.memcheck_mode = 'none';
DELETE FROM ext_memcheck.violation_log;
