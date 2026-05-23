/*-------------------------------------------------------------------------
 * pg_ext_memcheck
 *
 * Copyright (c) 2026, Samba Siva Reddy
 *
 * This software is released under the MIT License.
 * See LICENSE for details.
 *-------------------------------------------------------------------------
*/
#ifndef VIOLATION_LOG_H
#define VIOLATION_LOG_H

#include "postgres.h"
#include "fmgr.h"
#include "datatype/timestamp.h"
#include "storage/lwlock.h"

// Maximum number of violation entries to keep in the shared ring buffer at any time.
// Older entries will be overwritten when the buffer is full.
#define MEMCHECK_MAX_VIOLATIONS 2048

extern int violation_log_tranche_id;


// ViolationEntry represents a single memory violation detected by the extension.
typedef struct ViolationEntry {
    TimestampTz   ts;
    int           backend_pid;
    char          check_type[32];      // e.g., "context_leak", "wrong_ctx_alloc", etc.
    char          severity[16];        // e.g., "ERROR", "WARNING", "INFO", "OK"
    char          detail[256];         // Detailed message about the violation
    char          source_lib[64];      // basename of the .so that installed the leaking hook
} ViolationEntry;

// ViolationLog is a shared-memory structure that holds a circular buffer of ViolationEntry records.
typedef struct ViolationLog {
    LWLock       lock;                                 // Lightweight lock for thread safety
    int          count;                                // Number of logged violations
    int          head;                                 // Index of the next entry to write (circular buffer)
    ViolationEntry entries[MEMCHECK_MAX_VIOLATIONS];   // Circular buffer of violation entries
} ViolationLog;

// Helper functions to log violations
extern void violation_log_write(const char *check_type, const char *severity, const char *detail, const char *source_lib);
extern ViolationEntry *violation_log_read_all(void);

/*
 * violation_log_read_session -- drain entries matching backend_pid == pid and ts >= since.
 *
 * Returns a palloc'd array of matched entries; *out_count receives the count.
 * Matched slots are zeroed from the ring buffer atomically under an exclusive lock.
 * This gives memcheck_end() session-scoped, non-repeatable read semantics.
 */
extern ViolationEntry *violation_log_read_session(int pid, TimestampTz since, int *out_count);

// SQL-callable: flush shared-memory ring buffer into ext_memcheck.violation_log
extern Datum violation_log_flush(PG_FUNCTION_ARGS);

#endif /* VIOLATION_LOG_H */