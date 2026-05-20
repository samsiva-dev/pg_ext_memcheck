#ifndef SHMEM_PROBE_H
#define SHMEM_PROBE_H

#include "postgres.h"
#include "storage/lwlock.h"

#define SHMEM_PROBE_MAX_SEGMENTS 32
#define SHMEM_PROBE_SENTINEL_BYTE ((uint8) 0xDE)

typedef struct ProbeRecord {
    char  seg_name[64];
    Size  declared_size;
    bool  registered;
} ProbeRecord;

typedef struct ProbeRegistry {
    LWLock     lock;
    int        count;
    ProbeRecord records[SHMEM_PROBE_MAX_SEGMENTS];
} ProbeRegistry;

extern ProbeRegistry *probe_registry;
extern int shmem_probe_tranche_id;

extern void probe_register(const char *seg_name, Size declared_size);
extern bool probe_check(const char *seg_name);
extern void probe_check_all(void);
extern void probe_registry_clear(void);

#endif /* SHMEM_PROBE_H */
