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
#include "storage/ipc.h"
#include "storage/shmem.h"

// Local Includes
#include "include/pg_ext_memcheck.h"
#include "include/gucs.h"
#include "include/memcheck_hooks.h"
#include "include/violation_log.h"

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
}

// Extension unload callback
void
_PG_fini(void)
{
    elog(INFO, "pg_ext_memcheck unloaded");

    // Uninstall hooks
    uninstall_hooks();
    uninstall_executor_hooks();
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
                                                      sizeof(ViolationLog),
                                                      &found);
    
    if (!found)    {
        // First time initialization, zero out the log
        memset(violation_log, 0, sizeof(ViolationLog));
    }
}