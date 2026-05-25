/*-------------------------------------------------------------------------
 * pg_ext_memcheck
 *
 * Copyright (c) 2026, Samba Siva Reddy
 *
 * This software is released under the MIT License.
 * See LICENSE for details.
 *-------------------------------------------------------------------------
*/

#include "postgres.h"
#include "storage/dsm.h"
#include "storage/dsm_impl.h"
#include "miscadmin.h"
#include "utils/timestamp.h"

// Local Includes
#include "include/dsm_tracker.h"
#include "include/violation_log.h"
#include "include/memcheck_hooks.h"

int dsm_tracker_tranche_id = 0; /* initialized in _PG_init via LWLockNewTrancheId() */

void
dsm_tracker_check_leaks(void)
{
    int i;

    if (dsm_tracker_state == NULL)
        return; /* tracker not initialized, should not happen but be defensive */

    /*
     * Check for any segments that are still attached and were attached by the
     * current backend.  The `reported` flag prevents duplicate violations when
     * check_leaks() is called more than once for the same session (e.g. once at
     * end() and once at proc_exit).  We take an exclusive lock because we may
     * set the reported flag.
     */
    LWLockAcquire(&dsm_tracker_state->lock, LW_EXCLUSIVE);
    for (i = 0; i < dsm_tracker_state->count; i++)
    {
        DsmSegmentRecord *record = &dsm_tracker_state->segments[i];
        if (!record->detached && !record->reported &&
            record->backend_pid == MyProcPid && record->handle != 0)
        {
            char detail_msg[256];
            snprintf(detail_msg, sizeof(detail_msg),
                     "DSM segment with handle %u attached by backend PID %d at %s is still attached at session end. Size: %zu bytes.",
                     record->handle, record->backend_pid, timestamptz_to_str(record->attached_at), record->size_bytes);
            violation_log_write("dsm_leak", "WARNING", detail_msg, active_hook_libs);
            record->reported = true;
        }
    }
    LWLockRelease(&dsm_tracker_state->lock);
}

/*
 * dsm_tracker_record_handle_observe
 *
 * Record an externally-created DSM handle as not-yet-detached without
 * attaching our own reference or registering any detach callback.
 * Used by track_dsm_handle() so the record stays detached=false until
 * the user explicitly clears tracking or calls memcheck_end().
 */
void
dsm_tracker_record_handle_observe(dsm_handle handle, Size size_bytes)
{
    if (dsm_tracker_state == NULL)
        return;

    if (dsm_tracker_state->count >= DSM_TRACKER_MAX_SEGMENTS)
    {
        elog(WARNING, "DsmTrackerState is full; cannot track more segments");
        return;
    }

    LWLockAcquire(&dsm_tracker_state->lock, LW_EXCLUSIVE);
    {
        DsmSegmentRecord *record = &dsm_tracker_state->segments[dsm_tracker_state->count++];
        record->handle      = handle;
        record->backend_pid = MyProcPid;
        record->attached_at = GetCurrentTimestamp();
        record->size_bytes  = size_bytes;
        record->detached    = false;
        record->reported    = false;
    }
    LWLockRelease(&dsm_tracker_state->lock);
}


