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
    pg_ext_memcheck is a PostgreSQL extension that monitors memory usage during query execution
    and detects potential memory leaks or anomalies. It uses hooks into the executor to take
    snapshots of the memory context tree before and after query execution, compares them, and
    logs any detected issues to a shared violation log that can be queried via SQL.

    This file implements the hooks for monitoring memory usage during query execution.
*/

// Postgres Includes
#include "postgres.h"
#include "fmgr.h"
#include "optimizer/planner.h"
#include "executor/executor.h"
#include "utils/elog.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/shmem.h"

// Local Includes
#include "include/pg_ext_memcheck.h"
#include "include/gucs.h"
#include "include/memcheck_hooks.h"
#include "include/violation_log.h"
#include "include/dsm_tracker.h"
#include "include/shmem_probe.h"

void _PG_init(void);

/*
 * NOTE: _PG_fini is intentionally absent.  PostgreSQL never unloads
 * shared_preload_libraries modules, so an unload callback would be dead
 * code.  The on_proc_exit callback registered in _PG_init cannot be
 * deregistered; it runs harmlessly at backend exit for the lifetime of
 * the backend.
 */

PG_MODULE_MAGIC;

// Required Hook Types and Variables
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;

static void memcheck_shmem_request(void); // Forward declaration of shared memory request hook
static void memcheck_shmem_startup(void); // Forward declaration of shared memory startup hook

// Hook installation function

static void install_hooks(void);

ViolationLog *violation_log = NULL; // Global pointer to the shared violation log
DsmTrackerState *dsm_tracker_state = NULL; // Global pointer to the shared DSM tracker state
ProbeRegistry *probe_registry = NULL; // Global pointer to the shared probe registry
int shmem_probe_tranche_id = 0;

static void
memcheck_proc_exit_dsm_check(int code, Datum arg)
{
    /*
     * Safety-net: catch DSM segments still attached at backend exit that were
     * not caught by an explicit memcheck_end() call.
     */
    dsm_tracker_check_leaks();
}

// Extension load callback
void
_PG_init(void)
{
    elog(INFO, "pg_ext_memcheck loaded");

    // Define custom GUCs
    DefineCustomGUCs();

    // Install hooks
    install_hooks();
    install_executor_hooks();
    install_planner_hook();

    /* Initialize BGWorker crash-isolation harness */
    worker_harness_init();

    /* Register backend-exit callback to catch leaks even without an explicit end() */
    on_proc_exit(memcheck_proc_exit_dsm_check, (Datum) 0);
}

/*
    Hook installation function
    Installs hooks for planner, executor, logging, and shared memory startup. 
    Saves previous hooks to allow chaining.
*/
static void
install_hooks(void)
{
    // Save previous hooks and install our hooks

    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook = memcheck_shmem_request;

    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = memcheck_shmem_startup;
}

/*
 * SENTINEL_TEST_SEG_SIZE must NOT be CACHELINEALIGN-aligned (i.e. not a
 * multiple of the cache-line size, typically 64).  This guarantees that
 * CACHELINEALIGN(SENTINEL_TEST_SEG_SIZE) > SENTINEL_TEST_SEG_SIZE so the
 * sentinel byte at offset SENTINEL_TEST_SEG_SIZE lands inside the alignment
 * padding rather than past the allocator's reserved range.
 *
 * 10 bytes: CACHELINEALIGN(10)=64 on all common platforms, giving 54 bytes
 * of slack.  MAXALIGN(10)=16 on 64-bit, so byte 10 is in MAXALIGN padding.
 */
#define SENTINEL_TEST_SEG_NAME "pg_ext_memcheck SentinelTest"
#define SENTINEL_TEST_SEG_SIZE 10

static void
memcheck_shmem_request(void)
{
    if (prev_shmem_request_hook)
        prev_shmem_request_hook();

    RequestAddinShmemSpace(sizeof(ViolationLog) + 1);
    RequestAddinShmemSpace(sizeof(DsmTrackerState) + 1);
    RequestAddinShmemSpace(sizeof(ProbeRegistry));
    RequestAddinShmemSpace(sizeof(WorkerSlot));
    /* Small test segment for register_shmem_probe() integration tests. */
    RequestAddinShmemSpace(SENTINEL_TEST_SEG_SIZE);
}

static void memcheck_shmem_startup(void)
{
    // Allocate shared memory for the violation log
    bool found;

    /*
     * Tranche IDs must be allocated here (not in _PG_init) because
     * LWLockNewTrancheId() accesses a shared-memory counter that does
     * not exist until after CreateSharedMemoryAndSemaphores() runs.
     */
    if (violation_log_tranche_id == 0) 
    {
        violation_log_tranche_id = LWLockNewTrancheId();
        LWLockRegisterTranche(violation_log_tranche_id, "pg_ext_memcheck_violation_log");
    }
    if (dsm_tracker_tranche_id == 0)
    {
        dsm_tracker_tranche_id = LWLockNewTrancheId();
        LWLockRegisterTranche(dsm_tracker_tranche_id, "pg_ext_memcheck_dsm_tracker");
    }
    if (shmem_probe_tranche_id == 0)
    {
        shmem_probe_tranche_id = LWLockNewTrancheId();
        LWLockRegisterTranche(shmem_probe_tranche_id, "pg_ext_memcheck_shmem_probe");
    }
    if (worker_harness_tranche_id == 0)
    {
        worker_harness_tranche_id = LWLockNewTrancheId();
        LWLockRegisterTranche(worker_harness_tranche_id, "pg_ext_memcheck_worker_harness");
    }
    violation_log = (ViolationLog *) ShmemInitStruct("pg_ext_memcheck ViolationLog",
                                                      sizeof(ViolationLog) + 1,
                                                      &found);
    
    if (!found)    {
        // First time initialization, zero out the log
        memset(violation_log, 0, sizeof(ViolationLog));
        LWLockInitialize(&violation_log->lock, violation_log_tranche_id);
    }

    // Allocate shared memory for the DSM tracker state
    dsm_tracker_state = (DsmTrackerState *) ShmemInitStruct("pg_ext_memcheck DsmTrackerState",
                                                            sizeof(DsmTrackerState) + 1,
                                                            &found);
    if (!found) {
        // First time initialization, zero out the tracker state
        memset(dsm_tracker_state, 0, sizeof(DsmTrackerState));
        LWLockInitialize(&dsm_tracker_state->lock, dsm_tracker_tranche_id);
    }

    // Allocate shared memory for the shmem probe registry
    probe_registry = (ProbeRegistry *) ShmemInitStruct("pg_ext_memcheck ProbeRegistry",
                                                        sizeof(ProbeRegistry),
                                                        &found);
    if (!found) {
        memset(probe_registry, 0, sizeof(ProbeRegistry));
        LWLockInitialize(&probe_registry->lock, shmem_probe_tranche_id);
    }

    /*
     * Allocate the dedicated sentinel-test segment.  No struct needed; we
     * only reference it by name through probe_register() / probe_check().
     * The allocation must survive (not be optimised away), so we use the
     * returned pointer in a volatile write that keeps the call live.
     */
    {
        void *sentinel_test_ptr =
            ShmemInitStruct(SENTINEL_TEST_SEG_NAME, SENTINEL_TEST_SEG_SIZE, &found);
        if (!found)
            memset(sentinel_test_ptr, 0, SENTINEL_TEST_SEG_SIZE);
    }

    /* Allocate shared memory for the BGWorker crash-isolation harness */
    worker_slot = (WorkerSlot *) ShmemInitStruct("pg_ext_memcheck WorkerSlot",
                                                  sizeof(WorkerSlot), &found);
    if (!found) {
        memset(worker_slot, 0, sizeof(WorkerSlot));
        LWLockInitialize(&worker_slot->lock, worker_harness_tranche_id);
    }
}
