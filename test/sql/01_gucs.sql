-- 01_gucs.sql
-- Verify GUC defaults and accept all valid values without error.

SET client_min_messages = WARNING;

-- Default memcheck_mode should be 'all'
SHOW pg_ext_memcheck.memcheck_mode;

-- Default min_leak_bytes should be 8192
SHOW pg_ext_memcheck.min_leak_bytes;

-- Switch mode to 'executor'
SET pg_ext_memcheck.memcheck_mode = 'executor';
SHOW pg_ext_memcheck.memcheck_mode;

-- Switch mode to 'none'
SET pg_ext_memcheck.memcheck_mode = 'none';
SHOW pg_ext_memcheck.memcheck_mode;

-- Restore to default
SET pg_ext_memcheck.memcheck_mode = 'all';
SHOW pg_ext_memcheck.memcheck_mode;

-- Override min_leak_bytes
SET pg_ext_memcheck.min_leak_bytes = '16384';
SHOW pg_ext_memcheck.min_leak_bytes;

-- Reset to default
RESET pg_ext_memcheck.min_leak_bytes;
SHOW pg_ext_memcheck.min_leak_bytes;

-- Invalid mode value must error
DO $$
BEGIN
    SET pg_ext_memcheck.memcheck_mode = 'bogus_mode';
    RAISE NOTICE 'FAIL: should have errored';
EXCEPTION
    WHEN invalid_parameter_value THEN
        RAISE NOTICE 'OK: invalid mode rejected';
END;
$$;
