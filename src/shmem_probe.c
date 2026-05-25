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
#include "include/memcheck_hooks.h"


/*
 * probe_register
 *   seg_name   : name the segment is registered under in ShmemIndex
 *   alloc_size : exact size the owner allocated it with (so the lookup matches)
 *   data_end   : offset to plant the sentinel (must be < CACHELINEALIGN(alloc_size)).
 *                own segment padded with +1 -> data_end = alloc_size - 1 (reserved byte)
 *                foreign segment via SQL API -> data_end = alloc_size (uses align slack;
 *                  requires CACHELINEALIGN(alloc_size) > alloc_size, i.e. the size must
 *                  not already be cache-line-aligned — the SQL wrapper enforces this)
 */
void
probe_register(const char *seg_name, Size alloc_size, Size data_end)
{
    void *base_ptr;
    bool  found;
    Size  chunk = CACHELINEALIGN(alloc_size);   /* upper bound on bytes the allocator reserved */
    int   slot;

    if (probe_registry == NULL)
        return;

    /*
     * The sentinel must land strictly inside the cache-line-aligned chunk.
     * Using CACHELINEALIGN as the upper bound is conservative: PostgreSQL's
     * ShmemAlloc uses MAXALIGN (typically 8 bytes), so any data_end value
     * that passes this guard is also within the allocator's actual reservation
     * provided data_end < MAXALIGN(alloc_size) — which the SQL wrapper
     * guarantees by refusing non-slack sizes.
     */
    if (data_end >= chunk)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pg_ext_memcheck: '%s' has no slack for a sentinel "
                        "(data_end=%zu, chunk=%zu); allocate the segment with "
                        "an extra byte so the sentinel byte has room",
                        seg_name, data_end, chunk)));
    }

    /*
     * Look up with the EXACT allocation size:
     *  - found==true  -> returns the existing pointer, no re-alloc, no zeroing
     *  - wrong size   -> ShmemInitStruct ereport(ERROR)s ("entry size is wrong")
     *  - missing name -> it allocates a phantom (no public API removes it), so we refuse
     */
    base_ptr = ShmemInitStruct(seg_name, alloc_size, &found);
    if (!found)
    {
        elog(WARNING,
             "pg_ext_memcheck: segment '%s' did not pre-exist; not probing "
             "(a phantom ShmemIndex entry may now occupy this name)",
             seg_name);
        return;
    }

    LWLockAcquire(&probe_registry->lock, LW_EXCLUSIVE);

    /* Dedup: if this segment is already registered, update it in place. */
    for (slot = 0; slot < probe_registry->count; slot++)
    {
        if (strncmp(probe_registry->records[slot].seg_name, seg_name,
                    sizeof(probe_registry->records[slot].seg_name)) == 0)
        {
            probe_registry->records[slot].alloc_size = alloc_size;
            probe_registry->records[slot].data_end   = data_end;
            probe_registry->records[slot].registered = true;
            LWLockRelease(&probe_registry->lock);
            goto plant_sentinel;
        }
    }

    if (probe_registry->count >= SHMEM_PROBE_MAX_SEGMENTS)
    {
        LWLockRelease(&probe_registry->lock);
        elog(WARNING, "pg_ext_memcheck: probe registry full, cannot register '%s'", seg_name);
        return;
    }

    slot = probe_registry->count;
    strlcpy(probe_registry->records[slot].seg_name, seg_name,
            sizeof(probe_registry->records[slot].seg_name));
    probe_registry->records[slot].alloc_size = alloc_size;
    probe_registry->records[slot].data_end   = data_end;
    probe_registry->records[slot].registered = true;
    probe_registry->count++;
    LWLockRelease(&probe_registry->lock);

plant_sentinel:

    ((char *) base_ptr)[data_end] = (char) SHMEM_PROBE_SENTINEL_BYTE;
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
    Size  alloc_size = 0, data_end = 0;
    void *base_ptr;
    bool  shmem_found;

    LWLockAcquire(&probe_registry->lock, LW_SHARED);
    for (i = 0; i < probe_registry->count; i++)
    {
        ProbeRecord *r = &probe_registry->records[i];
        if (r->registered && strcmp(r->seg_name, seg_name) == 0)
        {
            alloc_size   = r->alloc_size;
            data_end     = r->data_end;
            found_record = true;
            break;
        }
    }
    LWLockRelease(&probe_registry->lock);
    if (!found_record)
        return false;

    base_ptr = ShmemInitStruct(seg_name, alloc_size, &shmem_found);   /* exact size again */
    if (!shmem_found)
        return false;

    return ((uint8 *) base_ptr)[data_end] == SHMEM_PROBE_SENTINEL_BYTE;
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
        declared_size = probe_registry->records[i].alloc_size;
        LWLockRelease(&probe_registry->lock);

        if (!probe_check(seg_name))
        {
            char detail[320];
            snprintf(detail, sizeof(detail),
                     "Sentinel byte overwritten past '%s' (declared_size=%zu)",
                     seg_name, declared_size);
            violation_log_write("shmem_overrun", "ERROR", detail, active_hook_libs);
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
