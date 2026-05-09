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
#include "utils/memutils.h"
#include "utils/palloc.h"

// Local Includes
#include "include/pg_ext_memcheck.h"
#include "include/gucs.h"
#include "include/memcheck_hooks.h"
#include "include/context_walker.h"

// Executor Hooks
static ExecutorStart_hook_type prev_executor_start_hook = NULL;
static ExecutorEnd_hook_type prev_executor_end_hook = NULL;

// Global Static Variables for Memory Snapshots
static CtxTree *before_snapshot = NULL;
static CtxTree *after_snapshot = NULL;

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

    if (memcheck_mode == MEMCHECK_EXECUTOR) {
        // Take before-snapshot
        before_snapshot = snapshot_context_tree(TopMemoryContext);
    }
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

    if (memcheck_mode == MEMCHECK_EXECUTOR) {
        int      diff_count = 0;
        CtxDiff *diffs      = NULL;

        // Take after-snapshot, compare with before-snapshot and log differences
        after_snapshot = snapshot_context_tree(TopMemoryContext);
        diffs = diff_context_trees(before_snapshot, after_snapshot, &diff_count);

        // Log the differences — iterate diff_count, not after_snapshot->count;
        // the diff array only contains matched contexts and may be shorter.
        for (int i = 0; i < diff_count; i++) {
            CtxDiff *d = &diffs[i];
            elog(LOG, "Memory context '%s' (depth %d): beforeAllocated=%zu, afterAllocated=%zu, beforeFree=%zu, afterFree=%zu",
                 d->name, d->depth, d->beforeAllocated, d->afterAllocated, d->beforeFree, d->afterFree);
        }

        // Free the snapshots and diffs
        free_context_tree(before_snapshot);
        free_context_tree(after_snapshot);
        free_context_diff(diffs);
        before_snapshot = NULL;
        after_snapshot  = NULL;
    }
}