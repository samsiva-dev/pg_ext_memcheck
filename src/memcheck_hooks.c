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
#include "optimizer/planner.h"

// Local Includes
#include "include/pg_ext_memcheck.h"
#include "include/gucs.h"
#include "include/memcheck_hooks.h"
#include "include/context_walker.h"
#include "include/violation_log.h"

// Planner Hooks
static planner_hook_type prev_planner_hook = NULL;

// Executor Hooks
static ExecutorStart_hook_type prev_executor_start_hook = NULL;
static ExecutorEnd_hook_type prev_executor_end_hook = NULL;

/* Before-snapshot for the current (non-internal) query. */
static CtxTree *before_snapshot = NULL;

// These contexts are globally known to be long-lived and shared across queries.
// So no wrongful allocations should happen directly in these contexts.
static const char *global_context_names[] = {
    "TopMemoryContext",
    "CacheMemoryContext",
    NULL
};

bool memcheck_in_internal_query = false;

// Helper function to determine if a context is a known global context or not
// We will use this when we are checking for wrong context allocations.
static bool 
is_global_context(const char *name) {
    for (int i = 0; global_context_names[i] != NULL; i++) {
        if (strcmp(name, global_context_names[i]) == 0) {
            return true;
        }
    }
    return false;
}

/*
    check_wrong_context_alloc: Checks for allocations in known global contexts and new contexts created 
    under global parents, which are common patterns of wrong context usage that can lead to memory leaks across queries.

    This function implements the following logic:
    Pass 1: growth in known global contexts 
    for each entry A in after->entries:
        if (!is_global_context(A.name)) continue;
        find matching B in before by (name, depth, parentHash)
        if B found and A.totalAllocated > B.totalAllocated:
            delta = A.totalAllocated - B.totalAllocated
            violation_log_write("wrong_ctx_alloc", "WARNING", detail_msg)

    Pass 2: new contexts created under a global parent 
    for each entry A in after->entries:
        if A is matched in before: continue   // not new
        find parent P in after where ctx_compute_hash(P.name, P.depth) == A.parentHash
        if P found and is_global_context(P.name):
            violation_log_write("wrong_ctx_alloc", "WARNING", detail_msg)
*/
void 
check_wrong_context_alloc(CtxTree *before, CtxTree *after)
{
    // Pass 1: growth in known global contexts
    for (int i = 0; i < after->count; i++)
    {
        CtxSnapshot *after_snapshot_entry = &after->entries[i];
        if (!is_global_context(after_snapshot_entry->name))
            continue;

        // Find matching entry in before by (name, depth, parentHash)
        CtxSnapshot *matched_before_snapshot_entry = NULL;
        for (int j = 0; j < before->count; j++)
        {
            CtxSnapshot *b = &before->entries[j];
            if (b->depth == after_snapshot_entry->depth &&
                b->parentHash == after_snapshot_entry->parentHash &&
                strcmp(b->name, after_snapshot_entry->name) == 0)
            {
                matched_before_snapshot_entry = b;
                break;
            }
        }

        // If a match is found and allocated memory grew, log a warning about potential wrong context usage
        if (matched_before_snapshot_entry != NULL &&
            after_snapshot_entry->totalAllocated > matched_before_snapshot_entry->totalAllocated)
        {
            Size delta = after_snapshot_entry->totalAllocated - matched_before_snapshot_entry->totalAllocated;
            char detail_msg[256];
            snprintf(detail_msg, sizeof(detail_msg),
                     "context '%s' (depth %d): allocated grew by %zu bytes in a known global context "
                     "(before=%zu after=%zu); free before=%zu after=%zu",
                     after_snapshot_entry->name, after_snapshot_entry->depth,
                     delta,
                     matched_before_snapshot_entry->totalAllocated, after_snapshot_entry->totalAllocated,
                     matched_before_snapshot_entry->totalFree, after_snapshot_entry->totalFree);
            violation_log_write("wrong_ctx_alloc", "WARNING", detail_msg);
        }
    }

    // Pass 2: new contexts created under a global parent
    for (int i = 0; i < after->count; i++)
    {
        CtxSnapshot *after_snapshot_entry = &after->entries[i];
        // Check if this entry is new (not matched in before)
        bool is_new = true;
        for (int j = 0; j < before->count; j++)
        {
            CtxSnapshot *b = &before->entries[j];
            if (b->depth == after_snapshot_entry->depth &&
                b->parentHash == after_snapshot_entry->parentHash &&
                strcmp(b->name, after_snapshot_entry->name) == 0)
            {                
                is_new = false;
                break;
            }
        }
        if (is_new)
        {
            // Find parent P in after where ctx_compute_hash(P.name, P.depth) == A.parentHash
            for (int j = 0; j < after->count; j++)
            {
                CtxSnapshot *P = &after->entries[j];
                if (ctx_compute_hash(P->name, P->depth) == after_snapshot_entry->parentHash &&
                    is_global_context(P->name))
                {
                    char detail_msg[256];
                    snprintf(detail_msg, sizeof(detail_msg),
                             "new context '%s' (depth %d) created under a global parent '%s' (depth %d)",
                             after_snapshot_entry->name, after_snapshot_entry->depth,
                             P->name, P->depth);
                    violation_log_write("wrong_ctx_alloc", "WARNING", detail_msg);
                    break;
                }
            }
        }
    }
}

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
void
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

void install_planner_hook(void) {
    prev_planner_hook = planner_hook;
    planner_hook = memcheck_planner_hook;
}

void uninstall_planner_hook(void) {
    planner_hook = prev_planner_hook;
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

    // For EXECUTOR/ALL mode, take an after-snapshot of the context tree at the end of the query execution,
    // compare it with the before-snapshot, and log any differences as violations.
    if ((memcheck_mode == MEMCHECK_EXECUTOR || memcheck_mode == MEMCHECK_ALL) && !memcheck_in_internal_query &&
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

        // Check for wrong context allocations in known global contexts and new contexts created under global parents, 
        // which are common patterns of wrong context usage that can lead to memory leaks across queries.
        check_wrong_context_alloc(before_snapshot, after);

        // Clean up snapshots and diffs to avoid memory leaks in the extension itself
        free_context_tree(before_snapshot);
        free_context_tree(after);
        free_context_diff(diffs);
        before_snapshot = NULL;
    }
}

// Memcheck Planner Hook Implementation
PlannedStmt *memcheck_planner_hook(Query *parse, const char *query_string, int cursorOptions, ParamListInfo boundParams) {
    // In ALL mode, Memory contexts allocated during planning are also tracked.
    // So we take a before-snapshot at the start of the planner hook, 
    // and the after-snapshot and diff analysis will be done in the ExecutorEnd hook.
    // Means ALL = PLANNER + EXECUTOR, while EXECUTOR = only Executor hooks.
    if (memcheck_mode == MEMCHECK_ALL && !memcheck_in_internal_query && before_snapshot == NULL) {
        // Record a snapshot of the memory context tree before planning starts, 
        // but only if we're in ALL mode and not already in an internal query
        before_snapshot = snapshot_context_tree(TopMemoryContext);
    }

    if (prev_planner_hook)
        return prev_planner_hook(parse, query_string, cursorOptions, boundParams);
    else
        return standard_planner(parse, query_string, cursorOptions, boundParams);
}