/*-------------------------------------------------------------------------
 * pg_ext_memcheck
 *
 * Copyright (c) 2026, Samba Siva Reddy
 *
 * This software is released under the MIT License.
 * See LICENSE for details.
 *-------------------------------------------------------------------------
*/

/*
    violation_log.c

    This file contains the implementation of the shared violation log for the pg_ext_memcheck extension.
    The violation log is a ring buffer stored in shared memory that records memory usage violations
    detected during query execution and related helper functions. The log can be queried via SQL functions.
*/

// Postgres Includes
#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"
#include "catalog/pg_type.h"

// Local Includes
#include "include/pg_ext_memcheck.h"
#include "include/gucs.h"
#include "include/violation_log.h"

#define VIOLATION_LOG_SIZE MEMCHECK_MAX_VIOLATIONS

// Logs a violation to the shared ViolationLog with the given details. Thread-safe via LWLock.
void
violation_log_write(const char *check_type, const char *severity, const char *detail)
{
    // violation_log should have been initialized in the shmem_startup_hook
    if (violation_log == NULL)
    {
        elog(WARNING, "Violation log not initialized; cannot log violation");
        return;
    }

    // Acquire lock to write to the log
    LWLockAcquire(&violation_log->lock, LW_EXCLUSIVE);

    // Write the violation entry at the current head position
    ViolationEntry *entry = &violation_log->entries[violation_log->head];
    entry->ts = GetCurrentTimestamp();
    entry->backend_pid = MyProcPid;
    strncpy(entry->check_type, check_type, sizeof(entry->check_type) - 1);
    entry->check_type[sizeof(entry->check_type) - 1] = '\0';
    strncpy(entry->severity, severity, sizeof(entry->severity) - 1);
    entry->severity[sizeof(entry->severity) - 1] = '\0';
    strncpy(entry->detail, detail, sizeof(entry->detail) - 1);
    entry->detail[sizeof(entry->detail) - 1] = '\0';

    // Update the head position
    violation_log->head = (violation_log->head + 1) % VIOLATION_LOG_SIZE;

    // Release the lock
    LWLockRelease(&violation_log->lock);
}

// Reads all violation entries from the shared ViolationLog. 
// Caller is responsible for freeing the returned array.
ViolationEntry *
violation_log_read_all()
{
    if (violation_log == NULL)
    {
        elog(WARNING, "Violation log not initialized; cannot read violations");
        return NULL;
    }

    ViolationEntry *entries = (ViolationEntry *) palloc(sizeof(ViolationEntry) * VIOLATION_LOG_SIZE);

    // Acquire lock to read from the log
    LWLockAcquire(&violation_log->lock, LW_SHARED);

    // Copy entries to the output array
    for (int i = 0; i < VIOLATION_LOG_SIZE; i++)
    {
        int index = (violation_log->head + i) % VIOLATION_LOG_SIZE;
        entries[i] = violation_log->entries[index];
    }

    // Release the lock
    LWLockRelease(&violation_log->lock);

    return entries;
}

/*
 * violation_log_flush -- SQL-callable function.
 *
 * Reads all entries from the shared-memory ring buffer and inserts each
 * non-empty entry into pg_ext_memcheck.violation_log via SPI.
 * Returns the number of rows inserted.
 */
PG_FUNCTION_INFO_V1(violation_log_flush);

Datum
violation_log_flush(PG_FUNCTION_ARGS)
{
    ViolationEntry *entries;
    int             inserted = 0;
    int             ret;

    entries = violation_log_read_all();
    if (entries == NULL)
        PG_RETURN_INT32(0);

    /* Suppress executor hook monitoring for our own SPI queries. */
    memcheck_in_internal_query = true;

    if (SPI_connect() != SPI_OK_CONNECT)
    {
        memcheck_in_internal_query = false;
        elog(ERROR, "pg_ext_memcheck: SPI_connect failed");
    }

    for (int i = 0; i < VIOLATION_LOG_SIZE; i++)
    {
        ViolationEntry *e = &entries[i];

        /* Skip slots that have never been written (check_type is empty). */
        if (e->check_type[0] == '\0')
            continue;

        Oid     argtypes[5] = { TIMESTAMPTZOID, INT4OID, TEXTOID, TEXTOID, TEXTOID };
        Datum   values[5];
        char    nulls[5]    = { ' ', ' ', ' ', ' ', ' ' };

        values[0] = TimestampTzGetDatum(e->ts);
        values[1] = Int32GetDatum(e->backend_pid);
        values[2] = CStringGetTextDatum(e->check_type);
        values[3] = CStringGetTextDatum(e->severity);
        values[4] = CStringGetTextDatum(e->detail);

        ret = SPI_execute_with_args(
            "INSERT INTO ext_memcheck.violation_log "
            "    (ts, backend_pid, check_type, severity, detail) "
            "VALUES ($1, $2, $3, $4, $5)",
            5, argtypes, values, nulls, false, 0);

        if (ret != SPI_OK_INSERT)
            elog(ERROR, "pg_ext_memcheck: INSERT failed (SPI error %d)", ret);

        inserted++;
    }

    SPI_finish();
    memcheck_in_internal_query = false;
    pfree(entries);

    /* Clear the shared-memory ring buffer after a successful flush. */
    LWLockAcquire(&violation_log->lock, LW_EXCLUSIVE);
    memset(violation_log->entries, 0, sizeof(violation_log->entries));
    violation_log->head  = 0;
    violation_log->count = 0;
    LWLockRelease(&violation_log->lock);

    PG_RETURN_INT32(inserted);
}