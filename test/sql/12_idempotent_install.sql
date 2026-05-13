-- 12_idempotent_install.sql
-- CREATE EXTENSION IF NOT EXISTS must be idempotent.

SET client_min_messages = WARNING;

CREATE EXTENSION IF NOT EXISTS pg_ext_memcheck;

-- Schema and table must still exist after second CREATE
SELECT schema_name
FROM information_schema.schemata
WHERE schema_name = 'ext_memcheck';

SELECT table_name
FROM information_schema.tables
WHERE table_schema = 'ext_memcheck'
  AND table_name   = 'violation_log';
