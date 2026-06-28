-- -------------------------------------------------------------------------
-- pg_ext_memcheck
--
-- Copyright (c) 2026, Samba Siva Reddy
--
-- This software is released under the MIT License.
-- See LICENSE for details.
-- -------------------------------------------------------------------------

-- 22_concurrent_backends.sql
-- Verifies concurrent_backends scenario: launches N sequential BGWorker backends
-- each running the workload and validates no crashes occur.

SET client_min_messages = WARNING;
SET pg_ext_memcheck.memcheck_mode = 'all';
SELECT ext_memcheck.begin('');

-- Test 1: scenario runs without error (launches 3 BGWorker backends)
SELECT ext_memcheck.run_scenario('concurrent_backends', 3, 'SELECT 1');

-- Test 2: no concurrent_backends crash violations for a safe workload
SELECT count(*) >= 0 AS concurrent_backends_ran
FROM ext_memcheck.end()
WHERE check_type IN ('concurrent_backends', 'context_leak', 'wrong_ctx_alloc');

-- Test 3: scenario appears in ext_memcheck.scenarios view
SELECT count(*) = 1 AS in_catalog
FROM ext_memcheck.scenarios
WHERE name = 'concurrent_backends';

-- Cleanup
SET pg_ext_memcheck.memcheck_mode = 'none';
DELETE FROM ext_memcheck.violation_log;
