-- 02_session_lifecycle.sql
-- Test begin/end session lifecycle: begin returns text, end returns empty set when
-- no monitoring has occurred in NONE mode.

SET client_min_messages = WARNING;

-- Put extension in NONE mode so no hook fires and ring buffer stays clean
SET pg_ext_memcheck.memcheck_mode = 'none';

-- Flush any leftover violations from previous tests
SELECT ext_memcheck.flush_violations();

-- Begin a session in 'none' mode
SELECT ext_memcheck.begin('none');

-- End the session - should return 0 rows because no violations were generated
SELECT count(*) AS violation_count FROM ext_memcheck.end();

-- Begin a session in 'executor' mode
SELECT ext_memcheck.begin('executor');
SHOW pg_ext_memcheck.memcheck_mode;

-- End the session
SELECT count(*) AS violation_count FROM ext_memcheck.end();

-- Confirm memcheck_mode is now 'none' after end()
SHOW pg_ext_memcheck.memcheck_mode;

-- Begin with unrecognised mode should default to 'all'
SELECT ext_memcheck.begin('unrecognised_mode');
SHOW pg_ext_memcheck.memcheck_mode;
-- Cleanup
SELECT ext_memcheck.begin('none');
SELECT count(*) FROM ext_memcheck.end();
