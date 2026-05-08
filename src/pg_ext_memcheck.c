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
#include "fmgr.h"

#include "optimizer/planner.h"
#include "executor/executor.h"
#include "utils/elog.h"
#include "storage/ipc.h"

void _PG_init(void);
void _PG_fini(void);

PG_MODULE_MAGIC;

// Required Hook Types and Variables
// (Add any necessary hooks here, e.g., ExecutorStart_hook, ExecutorEnd_hook, etc.)

static planner_hook_type prev_planner_hook = NULL;
static ExecutorStart_hook_type prev_executor_start_hook = NULL;
static ExecutorRun_hook_type prev_executor_run_hook = NULL;
static ExecutorEnd_hook_type prev_executor_end_hook = NULL;
static emit_log_hook_type prev_emit_log_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

// Hook installation and uninstallation functions

static void install_hooks(void);
static void uninstall_hooks(void);

// Extension load callback
void
_PG_init(void)
{
    elog(INFO, "pg_ext_memcheck loaded");

    // Install hooks
    install_hooks();
}

// Extension unload callback
void
_PG_fini(void)
{
    elog(INFO, "pg_ext_memcheck unloaded");

    // Uninstall hooks
    uninstall_hooks();
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
    prev_planner_hook = planner_hook;
    planner_hook = NULL;              // TODO: Set when defined

    prev_executor_start_hook = ExecutorStart_hook;
    ExecutorStart_hook = NULL;        // TODO: Set when defined

    prev_executor_run_hook = ExecutorRun_hook;
    ExecutorRun_hook = NULL;          // TODO: Set when defined

    prev_executor_end_hook = ExecutorEnd_hook;
    ExecutorEnd_hook = NULL;          // TODO: Set when defined

    prev_emit_log_hook = emit_log_hook;
    emit_log_hook = NULL;            // TODO: Set when defined

    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = NULL;        // TODO: Set when defined
}

/*
    Hook uninstallation function
    Restores previous hooks to ensure clean unload of the extension.
*/
static void
uninstall_hooks(void)
{
    // Restore previous hooks
    planner_hook = prev_planner_hook;
    ExecutorStart_hook = prev_executor_start_hook;
    ExecutorRun_hook = prev_executor_run_hook;
    ExecutorEnd_hook = prev_executor_end_hook;
    emit_log_hook = prev_emit_log_hook;
    shmem_startup_hook = prev_shmem_startup_hook;
}