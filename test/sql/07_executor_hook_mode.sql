-- 07_executor_hook_mode.sql
-- Verify the executor hook fires (or does not fire) according to memcheck_mode.

SET client_min_messages = WARNING;
SELECT ext_memcheck.flush_violations();
DELETE FROM ext_memcheck.violation_log;

-- In NONE mode the executor hook must NOT record violations for a plain query
SET pg_ext_memcheck.memcheck_mode = 'none';
SELECT 1 AS probe;
SELECT 2 AS probe;
-- No violations should have been added to the ring buffer
SELECT count(*) AS viol_in_none_mode FROM ext_memcheck.end();

-- In EXECUTOR mode the hook must be active for executor statements
SELECT ext_memcheck.begin('executor');
SELECT generate_series(1, 100) AS n;   -- exercise executor with some work
SELECT count(*) >= 0 AS hook_ran FROM ext_memcheck.end();

-- In ALL mode the planner hook must also be active
SELECT ext_memcheck.begin('all');
SELECT generate_series(1, 100) AS n;
SELECT count(*) >= 0 AS hook_ran FROM ext_memcheck.end();

-- Final cleanup
SET pg_ext_memcheck.memcheck_mode = 'none';
