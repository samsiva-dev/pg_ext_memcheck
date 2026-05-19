-- -------------------------------------------------------------------------
-- pg_ext_memcheck
--
-- Copyright (c) 2026, Samba Siva Reddy
--
-- This software is released under the MIT License.
-- See LICENSE for details.
-- -------------------------------------------------------------------------

-- 05_scenario_tx_abort_loop.sql
-- Run the tx_abort_loop scenario and verify it completes without error.

SET client_min_messages = WARNING;

SET pg_ext_memcheck.memcheck_mode = 'all';
SELECT ext_memcheck.flush_violations();
DELETE FROM ext_memcheck.violation_log;

-- Minimum iteration count (1)
SELECT ext_memcheck.run_scenario('tx_abort_loop', 1) IS NOT NULL AS ok;

-- Standard run with 10 iterations
SELECT ext_memcheck.run_scenario('tx_abort_loop', 10) IS NOT NULL AS ok;

-- Return message must mention 'Scenario executed'
SELECT ext_memcheck.run_scenario('tx_abort_loop', 1) LIKE '%Scenario executed%' AS msg_ok;

-- Zero iterations must raise an error
DO $$
BEGIN
    PERFORM ext_memcheck.run_scenario('tx_abort_loop', 0);
    RAISE NOTICE 'FAIL: should have errored';
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'OK: 0 iterations rejected';
END;
$$;

SET pg_ext_memcheck.memcheck_mode = 'none';
