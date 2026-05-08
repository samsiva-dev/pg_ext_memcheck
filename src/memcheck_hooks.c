/*-------------------------------------------------------------------------
 * pg_ext_memcheck
 *
 * Copyright (c) 2026, Samba Siva Reddy
 *
 * This software is released under the MIT License.
 * See LICENSE for details.
 *-------------------------------------------------------------------------
*/

// Postgres Includes
#include "postgres.h"
#include "fmgr.h"
#include "executor/executor.h"
#include "utils/elog.h"
#include "executor/execdesc.h"
#include "access/sdir.h"

// Local Includes
#include "include/pg_ext_memcheck.h"
#include "include/gucs.h"
#include "include/memcheck_hooks.h"

// Executor Hooks
static ExecutorStart_hook_type prev_executor_start_hook = NULL;
static ExecutorRun_hook_type prev_executor_run_hook = NULL;
static ExecutorEnd_hook_type prev_executor_end_hook = NULL;

// Install and Uninstall Hooks
void install_executor_hooks(void) {
    // Save previous hooks and install our hooks
    prev_executor_start_hook = ExecutorStart_hook;
    ExecutorStart_hook = memcheck_executor_start;

    prev_executor_run_hook = ExecutorRun_hook;
    ExecutorRun_hook = memcheck_executor_run;

    prev_executor_end_hook = ExecutorEnd_hook;
    ExecutorEnd_hook = memcheck_executor_end;
}

void uninstall_executor_hooks(void) {
    // Restore previous hooks
    ExecutorStart_hook = prev_executor_start_hook;
    ExecutorRun_hook = prev_executor_run_hook;
    ExecutorEnd_hook = prev_executor_end_hook;
}

// Memcheck Executor Start Hook Implementation
void memcheck_executor_start(QueryDesc *queryDesc, int eflags) {
    // Call the previous hook if it exists
    if (prev_executor_start_hook)
        prev_executor_start_hook(queryDesc, eflags);

    elog(INFO, "memcheck_executor_start called for query: %s", queryDesc->sourceText);
}

// Memcheck Executor Run Hook Implementation
void memcheck_executor_run(
    QueryDesc *queryDesc, ScanDirection direction, uint64 count, bool execute_once
) {
    // Call the previous hook if it exists
    if (prev_executor_run_hook)
        prev_executor_run_hook(queryDesc, direction, count, execute_once);
    
    elog(INFO, "memcheck_executor_run called for query: %s", queryDesc->sourceText);
}

// Memcheck Executor End Hook Implementation
void memcheck_executor_end(QueryDesc *queryDesc) {
    // Call the previous hook if it exists
    if (prev_executor_end_hook)
        prev_executor_end_hook(queryDesc);
    
    elog(INFO, "memcheck_executor_end called for query: %s", queryDesc->sourceText);
}