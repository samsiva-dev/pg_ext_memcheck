/*
 * shmem_probe.c
 *
 * Shmem sentinel probe for pg_ext_memcheck.
 *
 * Registers a sentinel byte (0xDE) just past the declared end of known shmem
 * segments.  After a workload completes, probe_check_all() verifies each
 * sentinel is still intact.  A corrupted sentinel means the extension under
 * test wrote past its declared shmem boundary.
 *
 * Safety: every segment registered here must have been allocated with
 * (declared_size + 1) bytes in RequestAddinShmemSpace() so that the sentinel
 * byte belongs to pg_ext_memcheck's own reserved space.
 */

#include "postgres.h"
#include "storage/shmem.h"
#include "storage/lwlock.h"
#include "utils/elog.h"

#include "include/shmem_probe.h"
#include "include/violation_log.h"

/*
 * probe_register
 *
 * Look up the shmem segment named seg_name (it must already exist), record it
 * in the ProbeRegistry, then write the sentinel byte at base_ptr[declared_size].
 */
void
probe_register(const char *seg_name, Size declared_size)
{
    void  *base_ptr;
    bool   found;
    int    slot;

    if (probe_registry == NULL)
        return;

    LWLockAcquire(&probe_registry->lock, LW_EXCLUSIVE);

    if (probe_registry->count >= SHMEM_PROBE_MAX_SEGMENTS)
    {
        LWLockRelease(&probe_registry->lock);
        elog(WARNING, "pg_ext_memcheck: probe registry full, cannot register '%s'",
             seg_name);
        return;
    }

    slot = probe_registry->count;
    LWLockRelease(&probe_registry->lock);

    /*
     * ShmemInitStruct with found=true just returns the existing pointer; it
     * does not re-allocate.  We need the segment to already exist.
     */
    base_ptr = ShmemInitStruct(seg_name, declared_size, &found);
    if (!found)
    {
        elog(WARNING,
             "pg_ext_memcheck: probe_register: segment '%s' not yet allocated, skipping",
             seg_name);
        return;
    }

    LWLockAcquire(&probe_registry->lock, LW_EXCLUSIVE);
    strlcpy(probe_registry->records[slot].seg_name, seg_name,
            sizeof(probe_registry->records[slot].seg_name));
    probe_registry->records[slot].declared_size = declared_size;
    probe_registry->records[slot].registered    = true;
    probe_registry->count++;
    LWLockRelease(&probe_registry->lock);

    /* Plant the sentinel byte in the extra reserved byte. */
    ((char *) base_ptr)[declared_size] = (char) SHMEM_PROBE_SENTINEL_BYTE;
}

/*
 * probe_check
 *
 * Returns true if the sentinel byte for seg_name is still intact.
 * Returns false if the record is not found or the byte was overwritten.
 */
bool
probe_check(const char *seg_name)
{
    int   i;
    bool  found_record = false;
    Size  declared_size = 0;
    void *base_ptr;
    bool  shmem_found;

    LWLockAcquire(&probe_registry->lock, LW_SHARED);
    for (i = 0; i < probe_registry->count; i++)
    {
        if (probe_registry->records[i].registered &&
            strcmp(probe_registry->records[i].seg_name, seg_name) == 0)
        {
            declared_size = probe_registry->records[i].declared_size;
            found_record  = true;
            break;
        }
    }
    LWLockRelease(&probe_registry->lock);

    if (!found_record)
        return false;

    base_ptr = ShmemInitStruct(seg_name, declared_size, &shmem_found);
    if (!shmem_found)
        return false;

    return ((uint8 *) base_ptr)[declared_size] == SHMEM_PROBE_SENTINEL_BYTE;
}

/*
 * probe_check_all
 *
 * Checks every registered segment.  For any whose sentinel has been
 * overwritten, writes a violation log entry.
 */
void
probe_check_all(void)
{
    int  i;
    int  count;

    if (probe_registry == NULL)
        return;

    LWLockAcquire(&probe_registry->lock, LW_SHARED);
    count = probe_registry->count;
    LWLockRelease(&probe_registry->lock);

    for (i = 0; i < count; i++)
    {
        char seg_name[64];
        Size declared_size;

        LWLockAcquire(&probe_registry->lock, LW_SHARED);
        if (!probe_registry->records[i].registered)
        {
            LWLockRelease(&probe_registry->lock);
            continue;
        }
        strlcpy(seg_name, probe_registry->records[i].seg_name, sizeof(seg_name));
        declared_size = probe_registry->records[i].declared_size;
        LWLockRelease(&probe_registry->lock);

        if (!probe_check(seg_name))
        {
            char detail[320];
            snprintf(detail, sizeof(detail),
                     "Sentinel byte overwritten past '%s' (declared_size=%zu)",
                     seg_name, declared_size);
            violation_log_write("shmem_overrun", "ERROR", detail, "");
        }
    }
}

/*
 * probe_registry_clear
 *
 * Resets the ProbeRegistry to empty.  Existing sentinel bytes in shmem are
 * left as-is; subsequent probe_register() calls will overwrite them.
 */
void
probe_registry_clear(void)
{
    if (probe_registry == NULL)
        return;

    LWLockAcquire(&probe_registry->lock, LW_EXCLUSIVE);
    memset(probe_registry->records, 0,
           sizeof(ProbeRecord) * probe_registry->count);
    probe_registry->count = 0;
    LWLockRelease(&probe_registry->lock);
}
