-- -------------------------------------------------------------------------
-- pg_ext_memcheck
--
-- Copyright (c) 2026, Samba Siva Reddy
--
-- This software is released under the MIT License.
-- See LICENSE for details.
-- -------------------------------------------------------------------------

-- 02_session_lifecycle.sql
-- Test begin/end session lifecycle: begin returns text, end returns empty set when
-- no monitoring has occurred in NONE mode.

SET client_min_messages = WARNING;

-- Put extension in NONE mode so no hook fires and ring buffer stays clean
SET pg_ext_memcheck.memcheck_mode = 'none';

-- Flush any leftover violations from previous tests
SELECT ext_memcheck.flush_violations() >= 0 AS cleaned;

-- Begin a session with no pattern (match all) while mode is 'none'
SELECT ext_memcheck.begin('');

-- End the session - should return 0 rows because no violations were generated
SELECT count(*) AS violation_count FROM ext_memcheck.end();

-- Begin a session in 'executor' mode
SET pg_ext_memcheck.memcheck_mode = 'executor';
SELECT ext_memcheck.begin('');
SHOW pg_ext_memcheck.memcheck_mode;

-- End the session
SELECT count(*) AS violation_count FROM ext_memcheck.end();

-- Confirm memcheck_mode is now 'none' after end()
SHOW pg_ext_memcheck.memcheck_mode;

-- begin() accepts a context pattern (SQL LIKE syntax)
SELECT ext_memcheck.begin('TestExt%') LIKE 'Memory check%' AS pattern_accepted;

-- begin() accepts a JSONB options block with allowlist
SELECT ext_memcheck.begin(
    'MyExt%',
    '{"track_shmem": true, "track_dsm": true, "allowed_contexts": ["TopMemoryContext"]}'
) LIKE 'Memory check%' AS options_accepted;

-- Cleanup
SET pg_ext_memcheck.memcheck_mode = 'none';
SELECT count(*) FROM ext_memcheck.end();
