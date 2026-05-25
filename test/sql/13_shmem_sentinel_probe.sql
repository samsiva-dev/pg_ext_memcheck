-- 13_shmem_sentinel_probe.sql
-- Verify shmem_sentinel_probe scenario runs without error,
-- appears in the scenarios view, and logs no shmem_overrun violations.
-- Also exercises the register_shmem_probe() SQL wrapper against the dedicated
-- 'pg_ext_memcheck SentinelTest' segment (10 bytes, allocated in _PG_init) so
-- that the sentinel is ACTUALLY planted and probe_check() can verify it.

SET client_min_messages = WARNING;

SELECT ext_memcheck.flush_violations() >= 0 AS flushed;
DELETE FROM ext_memcheck.violation_log;

-- Test A: a cache-line-aligned size must raise an explicit ERROR, not silently no-op.
-- (4096 is a multiple of 64, the typical cache-line size, so CACHELINEALIGN(4096)=4096
--  gives no alignment slack and the wrapper now rejects it outright.)
DO $$
BEGIN
    PERFORM ext_memcheck.register_shmem_probe('test_sentinel', 4096);
    RAISE EXCEPTION 'Expected an error for cache-line-aligned size but none was raised';
EXCEPTION
    WHEN OTHERS THEN
        -- Expected path: confirm the rejection message mentions the segment name.
        IF position('test_sentinel' IN SQLERRM) = 0 THEN
            RAISE EXCEPTION 'Error message did not mention segment name: %', SQLERRM;
        END IF;
        RAISE NOTICE 'Correctly rejected cache-line-aligned probe (4096 bytes): %', SQLERRM;
END;
$$ LANGUAGE plpgsql;

-- Test B: register a probe for the dedicated test segment (10 bytes, not CL-aligned).
-- CACHELINEALIGN(10) = 64, so bytes 10-63 are alignment padding and the sentinel
-- at offset 10 is safely within the allocator's reservation.
SELECT ext_memcheck.register_shmem_probe('pg_ext_memcheck SentinelTest', 10) IS NOT NULL AS probe_registered;

-- Test C: confirm the sentinel byte (0xDE) was actually planted.
SELECT ext_memcheck.probe_check('pg_ext_memcheck SentinelTest') AS sentinel_intact;

-- Clear registry so the scenario starts clean
SELECT ext_memcheck.clear_shmem_registry();

-- Scenario must run without error
SELECT ext_memcheck.run_scenario('shmem_sentinel_probe', 5) LIKE '%Scenario executed%' AS ok;

-- Scenario must appear in the scenarios view
SELECT name FROM ext_memcheck.scenarios WHERE name = 'shmem_sentinel_probe';

-- No shmem_overrun violations should be logged (sentinels should be intact)
SELECT count(*) AS overrun_count
FROM ext_memcheck.violation_log
WHERE check_type = 'shmem_overrun';
