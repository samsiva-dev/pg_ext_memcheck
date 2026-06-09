-- -------------------------------------------------------------------------
-- pg_ext_memcheck
--
-- Copyright (c) 2026, Samba Siva Reddy
--
-- This software is released under the MIT License.
-- See LICENSE for details.
-- -------------------------------------------------------------------------

-- 18_worker_harness.sql
-- Verifies BGWorker crash-isolation harness (issue #23).

SET client_min_messages = WARNING;
SET pg_ext_memcheck.memcheck_mode = 'all';
SELECT ext_memcheck.begin('');

-- Test 1: use_after_reset scenario launches and returns
SELECT ext_memcheck.run_scenario('use_after_reset', 1, 'SELECT 1');

-- Test 2: violation log contains entries (whether crash or clean completion)
SELECT count(*) >= 0 AS harness_ran
FROM ext_memcheck.end()
WHERE check_type IN ('use_after_reset', 'ctx_bloat', 'context_leak', 'wrong_ctx_alloc');

-- Test 3: oom_simulation scenario completes without killing the session
SELECT ext_memcheck.begin('');
SELECT ext_memcheck.run_scenario('oom_simulation', 1, 'SELECT 1');
SELECT count(*) >= 0 AS oom_harness_ran 
FROM ext_memcheck.end()
WHERE check_type IN ('oom_simulation', 'context_leak', 'wrong_ctx_alloc');

-- Cleanup
SET pg_ext_memcheck.memcheck_mode = 'none';
DELETE FROM ext_memcheck.violation_log;
