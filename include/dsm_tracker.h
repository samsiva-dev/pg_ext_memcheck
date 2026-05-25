/*-------------------------------------------------------------------------
 * pg_ext_memcheck
 *
 * Copyright (c) 2026, Samba Siva Reddy
 *
 * This software is released under the MIT License.
 * See LICENSE for details.
 *-------------------------------------------------------------------------
*/

#ifndef DSM_TRACKER_H
#define DSM_TRACKER_H

#include "postgres.h"
#include "fmgr.h"
#include "storage/dsm_impl.h"
#include "storage/lwlock.h"
#include "datatype/timestamp.h"

#define DSM_TRACKER_MAX_SEGMENTS 128

/* Structure to track DSM segments */
typedef struct DsmSegmentRecord {
    dsm_handle      handle;        /* DSM segment handle i.e., which segment this record corresponds to */
    int             backend_pid;   /* PID of the backend that owns this segment */
    TimestampTz     attached_at;   /* Timestamp when the segment was attached */
    Size            size_bytes;    /* Size of the DSM segment in bytes */
    bool            detached;      /* Flag indicating if the segment has been detached */
    bool            reported;      /* Flag indicating if this record has been logged by dsm_tracker_check_leaks() */
} DsmSegmentRecord;

/* Structure to track DSM segments state */
typedef struct DsmTrackerState {
    LWLock           lock;          /* Lock to protect access to the DSM tracker state */
    DsmSegmentRecord segments[DSM_TRACKER_MAX_SEGMENTS]; /* Array to hold records of observed DSM segments */
    int              count;     /* Number of currently tracked segments in the segments array, usually equal to DSM_TRACKER_MAX_SEGMENTS */
} DsmTrackerState;

// Global state for DSM tracking
extern DsmTrackerState *dsm_tracker_state;
extern int dsm_tracker_tranche_id;

// Helper functions for tracking DSM segments
extern void dsm_tracker_record_handle_observe(dsm_handle handle, Size size_bytes);
extern void dsm_tracker_check_leaks(void);   /* called from memcheck_end */
extern Datum dsm_tracker_list_segments(PG_FUNCTION_ARGS);

#endif /* DSM_TRACKER_H */