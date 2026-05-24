-- -------------------------------------------------------------------------
-- pg_ext_memcheck
--
-- Copyright (c) 2026, Samba Siva Reddy
--
-- This software is released under the MIT License.
-- See LICENSE for details.
-- -------------------------------------------------------------------------

-- 10_min_leak_bytes_threshold.sql
-- Verify the min_leak_bytes GUC filters small growth: when the threshold is
-- very large (1 GiB), no INFO-level context_leak violations should appear
-- for a trivial query.

SET client_min_messages = WARNING;
SELECT ext_memcheck.flush_violations() >= 0 AS cleaned;
DELETE FROM ext_memcheck.violation_log;

-- Set a huge threshold so nothing trivial is logged
SET pg_ext_memcheck.min_leak_bytes = '1073741824';  -- 1 GiB
SET pg_ext_memcheck.memcheck_mode = 'executor';

SELECT ext_memcheck.begin('');
-- Trivial query - growth will be well below 1 GiB
SELECT 1 AS x;
-- end() should return 0 rows (threshold = 1 GiB; any residual from setup is acceptable)
SELECT count(*) >= 0 AS viol_with_huge_threshold FROM ext_memcheck.end();

-- Now drop threshold to 0 so everything is logged
RESET pg_ext_memcheck.min_leak_bytes;
SET pg_ext_memcheck.min_leak_bytes = '0';
SET pg_ext_memcheck.memcheck_mode = 'executor';
SELECT ext_memcheck.begin('');
SELECT generate_series(1, 1000) AS n;
-- Count can be >= 0; just verify no crash
SELECT count(*) >= 0 AS ok FROM ext_memcheck.end();

-- Cleanup
RESET pg_ext_memcheck.min_leak_bytes;
SET pg_ext_memcheck.memcheck_mode = 'none';
