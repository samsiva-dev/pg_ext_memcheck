CREATE SCHEMA IF NOT EXISTS ext_memcheck;


/* 
    Table to store violation logs 
*/
CREATE TABLE IF NOT EXISTS ext_memcheck.violation_log (
    id SERIAL PRIMARY KEY,
    ts TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    backend_pid INTEGER NOT NULL,
    check_type TEXT NOT NULL,
    severity TEXT NOT NULL,
    detail TEXT NOT NULL
);

/*
    Flush the in-memory violation ring buffer into the violation_log table.
    Returns the number of rows inserted.
    Usage: SELECT ext_memcheck.flush_violations();
*/
CREATE OR REPLACE FUNCTION ext_memcheck.flush_violations()
RETURNS INTEGER
AS 'pg_ext_memcheck', 'violation_log_flush'
LANGUAGE C STRICT;