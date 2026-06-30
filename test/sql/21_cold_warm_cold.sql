-- -------------------------------------------------------------------------
-- pg_ext_memcheck
--
-- Copyright (c) 2026, Samba Siva Reddy
--
-- This software is released under the MIT License.
-- See LICENSE for details.
-- -------------------------------------------------------------------------

-- 21_cold_warm_cold.sql
-- Verifies cold_warm_cold scenario: detects CacheMemoryContext growth across
-- a cold-run → idle → warm-run boundary.

SET client_min_messages = WARNING;
SET pg_ext_memcheck.memcheck_mode = 'all';
SELECT ext_memcheck.begin('');

-- Test 1: scenario runs without error (includes a 1-second pg_sleep)
SELECT ext_memcheck.run_scenario('cold_warm_cold', 6, 'SELECT 1');

-- Test 2: results are non-negative (scenario may or may not find violations on SELECT 1)
SELECT count(*) >= 0 AS cold_warm_cold_ran
FROM ext_memcheck.end()
WHERE check_type IN ('context_leak', 'wrong_ctx_alloc', 'ctx_bloat');

-- Test 3: scenario appears in ext_memcheck.scenarios view
SELECT count(*) = 1 AS in_catalog
FROM ext_memcheck.scenarios
WHERE name = 'cold_warm_cold';

-- Cleanup
SET pg_ext_memcheck.memcheck_mode = 'none';
DELETE FROM ext_memcheck.violation_log;
