/*-------------------------------------------------------------------------
 * pg_ext_memcheck
 *
 * Copyright (c) 2026, Samba Siva Reddy
 *
 * This software is released under the MIT License.
 * See LICENSE for details.
 *-------------------------------------------------------------------------
*/

#ifndef SHMEM_PROBE_H
#define SHMEM_PROBE_H

#include "postgres.h"
#include "storage/lwlock.h"

// Maximum number of shared memory segments that can be tracked by the probe registry at once.
#define SHMEM_PROBE_MAX_SEGMENTS 32
// Sentinel byte value that pg_ext_memcheck writes just past the declared end of a shared memory 
// segment to detect overruns.
#define SHMEM_PROBE_SENTINEL_BYTE ((uint8) 0xDE)

// Record for each registered shared memory segment probe.
typedef struct ProbeRecord {
    char  seg_name[64];     /* name of the segment being probed (must match exactly the name used in ShmemInitStruct) */
    Size  alloc_size;       /* exact size the owner allocated it with (for lookup) */
    bool  registered;       /* whether this record is active; we keep old records around to detect duplicate registrations and avoid re-allocating sentinels */
    Size  data_end;         /* offset where the sentinel byte is planted, so that we can detect overruns from this point onward */
} ProbeRecord;

// Registry structure to hold all registered probes, protected by a lightweight lock for concurrent access.
typedef struct ProbeRegistry {
    LWLock     lock;         /* lock to protect access to the registry */
    int        count;        /* number of registered probes in the records array; always <= SHMEM_PROBE_MAX_SEGMENTS */
    ProbeRecord records[SHMEM_PROBE_MAX_SEGMENTS]; /* array of probe records; only the first 'count' entries are valid */
} ProbeRegistry;

// Global state for the probe registry and its associated LWLock tranche ID.
extern ProbeRegistry *probe_registry;
extern int shmem_probe_tranche_id;

// Functions to register a probe for a shared memory segment, check if the sentinel byte is intact for a segment, 
// check all registered segments, and clear the registry.

extern void probe_register(const char *seg_name, Size alloc_size, Size data_end);
extern bool probe_check(const char *seg_name);
extern void probe_check_all(void);
extern void probe_registry_clear(void);

#endif /* SHMEM_PROBE_H */
