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

static void dsm_tracker_on_detach_cb(dsm_segment *seg, Datum arg);

/*
    This function should be called whenever a new DSM segment is attached, to record it in our tracker.
    It extracts the handle and size of the segment, and stores it along with the attaching backend's PID and timestamp.
    It also registers a callback for when the segment is detached, so we can mark it as detached in our tracker.  
*/
void
dsm_tracker_record_attach(dsm_segment *seg, Size size_bytes)
{
    if (dsm_tracker_state == NULL)
        return; /* tracker not initialized, should not happen but be defensive */
    
    if (dsm_tracker_state->count >= DSM_TRACKER_MAX_SEGMENTS)
    {  
        elog(WARNING, "DsmTrackerState is full; cannot track more segments");
        return;
    }

    // Acquire lock to safely update the tracker state and insert the new segment record
    {
        dsm_handle handle = dsm_segment_handle(seg);
        LWLockAcquire(&dsm_tracker_state->lock, LW_EXCLUSIVE);
        {
            DsmSegmentRecord *record = &dsm_tracker_state->segments[dsm_tracker_state->count++];
            record->handle = handle;
            record->backend_pid = MyProcPid;
            record->attached_at = GetCurrentTimestamp();
            record->size_bytes = size_bytes;
            record->detached = false;
        }
        LWLockRelease(&dsm_tracker_state->lock);

        /* Register a callback for when this DSM segment is detached, so we can mark it as detached in our tracker. */
        on_dsm_detach(seg, dsm_tracker_on_detach_cb, UInt32GetDatum(handle));
    }
}

void 
dsm_tracker_check_leaks(void)
{
    if (dsm_tracker_state == NULL)
        return; /* tracker not initialized, should not happen but be defensive */

    // Check for any segments that are still attached and were attached by the current backend. 
    // Log a warning for each potential leak.
    LWLockAcquire(&dsm_tracker_state->lock, LW_EXCLUSIVE);
    for (int i = 0; i < dsm_tracker_state->count; i++)
    {
        DsmSegmentRecord *record = &dsm_tracker_state->segments[i];
        if (!record->detached && record->backend_pid == MyProcPid && record->handle != 0)
        {
            char detail_msg[256];
            snprintf(detail_msg, sizeof(detail_msg),
                     "DSM segment with handle %u attached by backend PID %d at %s is still attached after query completion. Size: %zu bytes.",
                     record->handle, record->backend_pid, timestamptz_to_str(record->attached_at), record->size_bytes);
            violation_log_write("dsm_leak", "WARNING", detail_msg, active_hook_libs);
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
    }
    LWLockRelease(&dsm_tracker_state->lock);
}

/*
    Callback function that is called when a tracked DSM segment is detached. 
    It marks the corresponding record in our tracker as detached.
*/
static void
dsm_tracker_on_detach_cb(dsm_segment *seg, Datum arg)
{
    dsm_handle handle = DatumGetUInt32(arg);
    for (int i = 0; i < dsm_tracker_state->count; i++)
    {
        DsmSegmentRecord *record = &dsm_tracker_state->segments[i];
        if (record->handle == handle && record->backend_pid == MyProcPid && !record->detached)
        {
            // Mark this segment as detached in the tracker
            record->detached = true;
            break;
        }
    }
}
