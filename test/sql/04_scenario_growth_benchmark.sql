-- -------------------------------------------------------------------------
-- pg_ext_memcheck
--
-- Copyright (c) 2026, Samba Siva Reddy
--
-- This software is released under the MIT License.
-- See LICENSE for details.
-- -------------------------------------------------------------------------

-- 04_scenario_growth_benchmark.sql
-- Run the growth_benchmark scenario and verify it completes without error.

SET client_min_messages = WARNING;

SET pg_ext_memcheck.memcheck_mode = 'all';
SELECT ext_memcheck.flush_violations();
DELETE FROM ext_memcheck.violation_log;

-- Minimum iteration count (1) must work
SELECT ext_memcheck.run_scenario('growth_benchmark', 1) LIKE '%Scenario executed%' AS ok;

-- Standard run with 10 iterations
SELECT ext_memcheck.run_scenario('growth_benchmark', 10) LIKE '%Scenario executed%' AS ok;

-- Return message must mention 'Scenario executed'
SELECT ext_memcheck.run_scenario('growth_benchmark', 1) LIKE '%Scenario executed%' AS msg_ok;

-- Zero or negative iterations must raise an error
DO $$
BEGIN
    PERFORM ext_memcheck.run_scenario('growth_benchmark', 0);
    RAISE NOTICE 'FAIL: should have errored on 0 iterations';
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'OK: 0 iterations rejected';
END;
$$;

DO $$
BEGIN
    PERFORM ext_memcheck.run_scenario('growth_benchmark', -1);
    RAISE NOTICE 'FAIL: should have errored on -1 iterations';
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'OK: negative iterations rejected';
END;
$$;

SET pg_ext_memcheck.memcheck_mode = 'none';
