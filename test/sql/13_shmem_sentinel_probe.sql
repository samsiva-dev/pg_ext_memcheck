-- 13_shmem_sentinel_probe.sql
-- Verify shmem_sentinel_probe scenario runs without error,
-- appears in the scenarios view, and logs no shmem_overrun violations.

SET client_min_messages = WARNING;

SELECT ext_memcheck.flush_violations();
DELETE FROM ext_memcheck.violation_log;

-- Scenario must run without error
SELECT ext_memcheck.run_scenario('shmem_sentinel_probe', 5) IS NOT NULL AS ok;

-- Scenario must appear in the scenarios view
SELECT name FROM ext_memcheck.scenarios WHERE name = 'shmem_sentinel_probe';

-- No shmem_overrun violations should be logged (sentinels should be intact)
SELECT count(*) AS overrun_count
FROM ext_memcheck.violation_log
WHERE check_type = 'shmem_overrun';
