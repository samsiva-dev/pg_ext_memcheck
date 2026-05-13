-- 11_violation_log_schema.sql
-- Verify column types and constraints of ext_memcheck.violation_log.

SET client_min_messages = WARNING;

-- Column list and data types
SELECT column_name, data_type, is_nullable
FROM information_schema.columns
WHERE table_schema = 'ext_memcheck'
  AND table_name   = 'violation_log'
ORDER BY ordinal_position;

-- Primary key exists
SELECT count(*) AS pk_count
FROM information_schema.table_constraints
WHERE table_schema   = 'ext_memcheck'
  AND table_name     = 'violation_log'
  AND constraint_type = 'PRIMARY KEY';

-- ts column has a DEFAULT
SELECT column_default IS NOT NULL AS has_default
FROM information_schema.columns
WHERE table_schema = 'ext_memcheck'
  AND table_name   = 'violation_log'
  AND column_name  = 'ts';
