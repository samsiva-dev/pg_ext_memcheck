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

typedef struct DsmSegmentRecord {
    dsm_handle      handle;
    int             backend_pid;
    TimestampTz     attached_at;
    Size            size_bytes;
    bool            detached;
} DsmSegmentRecord;

typedef struct DsmTrackerState {
    LWLock           lock;
    DsmSegmentRecord segments[DSM_TRACKER_MAX_SEGMENTS];
    int              count;
} DsmTrackerState;

extern DsmTrackerState *dsm_tracker_state;
extern int dsm_tracker_tranche_id;
extern void dsm_tracker_record_attach(dsm_segment *seg, Size size_bytes);
extern void dsm_tracker_record_handle_observe(dsm_handle handle, Size size_bytes);
extern void dsm_tracker_check_leaks(void);   /* called from memcheck_end */
extern Datum dsm_tracker_list_segments(PG_FUNCTION_ARGS);

#endif /* DSM_TRACKER_H */