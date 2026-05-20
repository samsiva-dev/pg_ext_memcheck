-- -------------------------------------------------------------------------
-- pg_ext_memcheck
--
-- Copyright (c) 2026, Samba Siva Reddy
--
-- This software is released under the MIT License.
-- See LICENSE for details.
-- -------------------------------------------------------------------------

-- 00_setup.sql
-- Load extension and verify schema/objects are created.

SET client_min_messages = WARNING;
CREATE EXTENSION IF NOT EXISTS pg_ext_memcheck;

-- Confirm schema exists
SELECT schema_name
FROM information_schema.schemata
WHERE schema_name = 'ext_memcheck';

-- Confirm violation_log table exists
SELECT table_name
FROM information_schema.tables
WHERE table_schema = 'ext_memcheck'
  AND table_name   = 'violation_log';

-- Confirm SQL API functions exist
SELECT routine_name
FROM information_schema.routines
WHERE routine_schema = 'ext_memcheck'
  AND routine_name IN ('begin', 'end', 'flush_violations', 'run_scenario')
ORDER BY routine_name;
