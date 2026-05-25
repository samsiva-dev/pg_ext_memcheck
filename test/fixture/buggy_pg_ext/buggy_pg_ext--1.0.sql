-- buggy_pg_ext--1.0.sql
--
-- The extension is hook-based: all functionality is registered in _PG_init()
-- when the shared library is loaded via shared_preload_libraries.

-- Complain if sourced directly in psql rather than through CREATE EXTENSION.
\echo Use "CREATE EXTENSION buggy_pg_ext" to load this file. \quit

-- Returns the dsm_handle of the most recently leaked DSM segment as int8.
-- Use with the pg_ext_memcheck tracking harness:
--
--   SELECT ext_memcheck.track_dsm_handle(buggy_last_dsm_handle());
--
-- Returns 0 (DSM_HANDLE_INVALID) if no segment has been created yet in
-- this backend session.
CREATE FUNCTION buggy_last_dsm_handle()
    RETURNS int8
    LANGUAGE C STRICT
    AS 'MODULE_PATHNAME', 'buggy_last_dsm_handle';
