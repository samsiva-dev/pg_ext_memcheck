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
    memcheck_hooks.c
    
    This file contains the implementation of the hooks for the pg_ext_memcheck extension.
    These hooks are used to monitor memory usage during query execution and log any
    detected memory leaks or anomalies to a shared violation log.
    
    The main hooks implemented here are:
    - ExecutorStart_hook: Takes a snapshot of the memory context tree before query execution.
    - ExecutorEnd_hook: Takes another snapshot after query execution, compares it with the before-snapshot,
      and logs any differences as violations in the shared violation log.
    
    The hooks respect the configured memcheck_mode and only perform monitoring when appropriate.
*/

// Postgres Includes
#include "postgres.h"
#include "fmgr.h"
#include "executor/executor.h"
#include "utils/elog.h"
#include "executor/execdesc.h"
#include "access/sdir.h"
#include "utils/memutils.h"
#include "utils/palloc.h"

// Local Includes
#include "include/pg_ext_memcheck.h"
#include "include/gucs.h"
#include "include/memcheck_hooks.h"
#include "include/context_walker.h"
#include "include/violation_log.h"

// Executor Hooks
static ExecutorStart_hook_type prev_executor_start_hook = NULL;
static ExecutorEnd_hook_type prev_executor_end_hook = NULL;

/* Before-snapshot for the current (non-internal) query. */
static CtxTree *before_snapshot = NULL;

bool memcheck_in_internal_query = false;

/*
 * analyze_and_log_diff
 *
 * Inspects a single CtxDiff entry, determines the severity of any memory
 * growth, and writes a ViolationEntry to the shared violation_log ring buffer.
 *
 * Severity thresholds (based on bytes allocated since before-snapshot):
 *   ERROR   : growth > 1 MiB  — likely a large leak or runaway allocation
 *   WARNING : growth > 64 KiB — moderate allocation retained across the query
 *   INFO    : any positive growth — small but noteworthy retained allocation
 *
 * Contexts whose allocated size did not increase are silently skipped.
 */
static void
analyze_and_log_diff(CtxDiff *diff)
{
    Size        delta_allocated;
    const char *severity;
    char        detail[256];

    /* Only report contexts where allocated memory grew after the query */
    if (diff->afterAllocated <= diff->beforeAllocated)
        return;

    delta_allocated = diff->afterAllocated - diff->beforeAllocated;

    if (delta_allocated > (Size)(1 * 1024 * 1024))        /* > 1 MiB */
        severity = "ERROR";
    else if (delta_allocated > (Size)(64 * 1024))          /* > 64 KiB */
        severity = "WARNING";
    else if (delta_allocated >= (Size) memcheck_min_leak_bytes)
        severity = "INFO";
    else
        return; /* below the configured threshold, skip silently */

    snprintf(detail, sizeof(detail),
             "context '%s' (depth %d): allocated grew by %zu bytes "
             "(before=%zu after=%zu); free before=%zu after=%zu",
             diff->name, diff->depth,
             delta_allocated,
             diff->beforeAllocated, diff->afterAllocated,
             diff->beforeFree, diff->afterFree);

    // Finally, write the violation to the shared log
    violation_log_write("context_leak", severity, detail);
}

// Install and Uninstall Hooks
void install_executor_hooks(void) {
    // Save previous hooks and install our hooks
    prev_executor_start_hook = ExecutorStart_hook;
    ExecutorStart_hook = memcheck_executor_start;

    prev_executor_end_hook = ExecutorEnd_hook;
    ExecutorEnd_hook = memcheck_executor_end;
}

void uninstall_executor_hooks(void) {
    // Restore previous hooks
    ExecutorStart_hook = prev_executor_start_hook;
    ExecutorEnd_hook = prev_executor_end_hook;
}

/*
    What need to be done in the hooks:
    On ExecutorStart: 
        if memcheck_mode == MEMCHECK_EXECUTOR and the query is not a memcheck internal query, take the before-snapshot.
    On ExecutorEnd: 
        if memcheck_mode == MEMCHECK_EXECUTOR and the query is not a memcheck internal query, take the after-snapshot, 
        compare with before-snapshot and log any differences.
*/

// Memcheck Executor Start Hook Implementation
void memcheck_executor_start(QueryDesc *queryDesc, int eflags) {
    // Call the previous hook if it exists
    if (prev_executor_start_hook)
        prev_executor_start_hook(queryDesc, eflags);
    else {
        // Call the core ExecutorStart if no previous hook exists
        standard_ExecutorStart(queryDesc, eflags); 
    }

    // Skip memory checking if mode is NONE
    if (memcheck_mode == MEMCHECK_NONE) {
        return; 
    }

    // For EXECUTOR mode, take a before-snapshot of the context tree 
    // at the start of the query execution.
    if (memcheck_mode == MEMCHECK_EXECUTOR && !memcheck_in_internal_query)
        before_snapshot = snapshot_context_tree(TopMemoryContext);
}

// Memcheck Executor End Hook Implementation
void memcheck_executor_end(QueryDesc *queryDesc) {
    // Call the previous hook if it exists
    if (prev_executor_end_hook)
        prev_executor_end_hook(queryDesc);
    else {
        // Call the core ExecutorEnd if no previous hook exists
        standard_ExecutorEnd(queryDesc); 
    }
    
    // Skip memory checking if mode is NONE
    if (memcheck_mode == MEMCHECK_NONE) {
        return; 
    }

    // For EXECUTOR mode, take an after-snapshot of the context tree at the end of the query execution,
    // compare it with the before-snapshot, and log any differences as violations.
    if (memcheck_mode == MEMCHECK_EXECUTOR && !memcheck_in_internal_query &&
        before_snapshot != NULL)
    {
        int      diff_count = 0;
        CtxDiff *diffs      = NULL;
        CtxTree *after      = snapshot_context_tree(TopMemoryContext);

        // Compute the differences between before and after snapshots
        diffs = diff_context_trees(before_snapshot, after, &diff_count);

        /* Analyze each diff entry and write qualifying violations to shared memory */
        for (int i = 0; i < diff_count; i++)
            analyze_and_log_diff(&diffs[i]);

        free_context_tree(before_snapshot);
        free_context_tree(after);
        free_context_diff(diffs);
        before_snapshot = NULL;
    }
}