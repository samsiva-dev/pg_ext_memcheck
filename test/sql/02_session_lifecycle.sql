-- -------------------------------------------------------------------------
-- pg_ext_memcheck
--
-- Copyright (c) 2026, Samba Siva Reddy
--
-- This software is released under the MIT License.
-- See LICENSE for details.
-- -------------------------------------------------------------------------

-- 02_session_lifecycle.sql
-- Test begin/end session lifecycle: begin() activates monitoring (defaults to ALL
-- mode if none was configured), end() returns the session's violations and forces
-- mode back to NONE so the next begin() starts a clean window.

SET client_min_messages = WARNING;

-- Flush any leftover violations from previous tests
SELECT ext_memcheck.flush_violations() >= 0 AS cleaned;

-- begin() with mode=NONE (the default) activates MEMCHECK_ALL automatically.
-- No test workload between begin/end, so violation count must be 0.
SELECT ext_memcheck.begin('');
SELECT count(*) AS violation_count FROM ext_memcheck.end();

-- Confirm memcheck_mode is 'none' after end() closes the window
SHOW pg_ext_memcheck.memcheck_mode;

-- begin() with a pre-SET mode honours the caller's choice.
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
