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
void _PG_fini(void);

PG_MODULE_MAGIC;

// Required Hook Types and Variables
// (Add any necessary hooks here, e.g., ExecutorStart_hook, ExecutorEnd_hook, etc.)
static emit_log_hook_type prev_emit_log_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

static void memcheck_shmem_startup(void); // Forward declaration of shared memory startup hook

// Hook installation and uninstallation functions

static void install_hooks(void);
static void uninstall_hooks(void);

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

    RequestAddinShmemSpace(sizeof(ViolationLog) + 1);
    RequestAddinShmemSpace(sizeof(DsmTrackerState) + 1);
    RequestAddinShmemSpace(sizeof(ProbeRegistry));

    dsm_tracker_tranche_id = LWLockNewTrancheId();
    LWLockRegisterTranche(dsm_tracker_tranche_id, "pg_ext_memcheck_dsm_tracker");

    shmem_probe_tranche_id = LWLockNewTrancheId();
    LWLockRegisterTranche(shmem_probe_tranche_id, "pg_ext_memcheck_shmem_probe");

    /* Register backend-exit callback to catch leaks even without an explicit end() */
    on_proc_exit(memcheck_proc_exit_dsm_check, (Datum) 0);
}

// Extension unload callback
void
_PG_fini(void)
{
    elog(INFO, "pg_ext_memcheck unloaded");

    // Uninstall hooks
    uninstall_hooks();
    uninstall_executor_hooks();
    uninstall_planner_hook();
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

    prev_emit_log_hook = emit_log_hook;
    emit_log_hook = NULL;            // TODO: Set when defined

    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = memcheck_shmem_startup;
}

/*
    Hook uninstallation function
    Restores previous hooks to ensure clean unload of the extension.
*/
static void
uninstall_hooks(void)
{
    // Restore previous hooks
    emit_log_hook = prev_emit_log_hook;
    shmem_startup_hook = prev_shmem_startup_hook;
}

static void memcheck_shmem_startup(void)
{
    // Allocate shared memory for the violation log
    bool found;
    violation_log = (ViolationLog *) ShmemInitStruct("pg_ext_memcheck ViolationLog",
                                                      sizeof(ViolationLog) + 1,
                                                      &found);
    
    if (!found)    {
        // First time initialization, zero out the log
        memset(violation_log, 0, sizeof(ViolationLog));
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
}