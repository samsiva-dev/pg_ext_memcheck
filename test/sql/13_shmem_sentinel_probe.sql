-- 13_shmem_sentinel_probe.sql
-- Verify shmem_sentinel_probe scenario runs without error,
-- appears in the scenarios view, and logs no shmem_overrun violations.
-- Also exercises the register_shmem_probe() SQL wrapper directly so that
-- a RETURNS TEXT / PG_RETURN_VOID mismatch (SIGSEGV) is caught here.

SET client_min_messages = WARNING;

SELECT ext_memcheck.flush_violations() >= 0 AS flushed;
DELETE FROM ext_memcheck.violation_log;

-- Direct SQL wrapper call: must return a non-null TEXT confirmation message.
-- If the C function incorrectly returns VOID (Datum 0) the detoast of the
-- null pointer triggers SIGSEGV; this test catches that at the SQL level.
SELECT ext_memcheck.register_shmem_probe('test_sentinel', 4096) IS NOT NULL AS probe_registered;

-- Clear registry so the scenario starts clean
SELECT ext_memcheck.clear_shmem_registry();

-- Scenario must run without error
SELECT ext_memcheck.run_scenario('shmem_sentinel_probe', 5) IS NOT NULL AS ok;

-- Scenario must appear in the scenarios view
SELECT name FROM ext_memcheck.scenarios WHERE name = 'shmem_sentinel_probe';

-- No shmem_overrun violations should be logged (sentinels should be intact)
SELECT count(*) AS overrun_count
FROM ext_memcheck.violation_log
WHERE check_type = 'shmem_overrun';
