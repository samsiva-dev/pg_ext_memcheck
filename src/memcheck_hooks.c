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
#include <dlfcn.h>  /* dladdr() for hook-library attribution */
#include <string.h>

// Local Includes
#include "include/pg_ext_memcheck.h"
#include "include/gucs.h"
#include "include/memcheck_hooks.h"
#include "include/context_walker.h"
#include "include/violation_log.h"
#include "include/dsm_tracker.h"

// Planner Hooks
static planner_hook_type prev_planner_hook = NULL;

// Executor Hooks
static ExecutorStart_hook_type prev_executor_start_hook = NULL;
static ExecutorEnd_hook_type prev_executor_end_hook = NULL;

/*
 * Fixed memory context for before_snapshot allocations.
 *
 * Created under TopMemoryContext so it outlives any individual query.
 * We switch into this context before calling snapshot_context_tree() for the
 * before-snapshot so that the palloc'd CtxTree and its entries are always in
 * the same, deterministic, long-lived context — regardless of whether the
 * snapshot is taken inside planner_hook (CurrentMemoryContext == MessageContext)
 * or inside ExecutorStart_hook (CurrentMemoryContext varies).
 */
static MemoryContext memcheck_hooks_ctx = NULL;

/* Before-snapshot for the current (non-internal) query.
 *
 * LIMITATION (nested queries): This is a single pointer, not a stack.  When a
 * PL/pgSQL function (or any nested SQL invocation) triggers ExecutorStart/End
 * recursively, the inner ExecutorEnd will pfree() before_snapshot and set it to
 * NULL.  The outer ExecutorEnd then finds before_snapshot == NULL and silently
 * skips analysis for the outer query.  In practice this means memory checks are
 * only reliable for leaf (non-nested) queries within a single backend.  A future
 * Phase 2 fix would replace this pointer with a stack (MemoryContext-allocated
 * list) so each nesting level preserves its own before-snapshot.
 * 
 * ===== Memory Context Note =====
 * We have to take the before-snapshot in MemCheckHooksContext context under TopMemoryContext
 * because before_snapshot will be palloc'd in various places like planner_hook, ExecutorStart_hook.
 * And planner_hook is under MessageContext and ExecutorStart_hook is under some other context making
 * before-snapshot's context non-deterministic if we allocate it in the hook itself.  By using a fixed context
 * for before_snapshot, we ensure it is always allocated in a long-lived context that outlives the hooks 
 * and the query execution.
 */
static CtxTree *before_snapshot = NULL;

/*
 * Library basename(s) of the hooks that were installed before ours, resolved
 * via dladdr().  Populated each time we take a before-snapshot so that every
 * violation written during that query carries attribution.
 */
char active_hook_libs[128] = "";

/*
 * resolve_active_hook_libs
 *
 * Walk the full hook chain — both the current global hook pointers and our
 * saved prev_* pointers — through dladdr() to find which shared libraries
 * have hooks installed.  Our own library is excluded.  Stores a
 * comma-separated list of basenames in active_hook_libs.
 *
 * This handles both load orderings:
 *   - pg_ext_memcheck loaded first: planner_hook == buggy's fn (prev_* are NULL)
 *   - pg_ext_memcheck loaded last:  planner_hook == our fn (filtered out),
 *                                    prev_* == buggy's fn
 */
static void
resolve_active_hook_libs(void)
{
    Dl_info     self_info;
    Dl_info     info;
    const char *self_fname = NULL;
    /* candidates: current globals first so load-order-first case is covered */
    const void *candidates[6];
    int         n_candidates;
    char        seen[4][64];
    int         n_seen = 0;
    char        buf[128] = "";
    int         i, j;

    candidates[0] = (const void *)(uintptr_t) planner_hook;
    candidates[1] = (const void *)(uintptr_t) ExecutorStart_hook;
    candidates[2] = (const void *)(uintptr_t) ExecutorEnd_hook;
    candidates[3] = (const void *)(uintptr_t) prev_planner_hook;
    candidates[4] = (const void *)(uintptr_t) prev_executor_start_hook;
    candidates[5] = (const void *)(uintptr_t) prev_executor_end_hook;
    n_candidates  = 6;

    /* Determine our own library path so we can exclude it */
    if (dladdr((const void *)(uintptr_t) resolve_active_hook_libs, &self_info) != 0)
        self_fname = self_info.dli_fname;

    for (i = 0; i < n_candidates; i++)
    {
        const char *base;
        const char *name;
        bool        dup = false;

        if (candidates[i] == NULL)
            continue;
        if (dladdr(candidates[i], &info) == 0 || info.dli_fname == NULL)
            continue;
        /* Skip our own library */
        if (self_fname != NULL && strcmp(info.dli_fname, self_fname) == 0)
            continue;

        base = strrchr(info.dli_fname, '/');
        name = base ? base + 1 : info.dli_fname;

        /* Deduplicate */
        for (j = 0; j < n_seen; j++)
            if (strcmp(seen[j], name) == 0) { dup = true; break; }
        if (dup || n_seen >= 4)
            continue;

        strncpy(seen[n_seen], name, sizeof(seen[n_seen]) - 1);
        seen[n_seen][sizeof(seen[n_seen]) - 1] = '\0';
        n_seen++;

        if (buf[0] != '\0')
        {
            size_t used = strlen(buf);
            snprintf(buf + used, sizeof(buf) - used, ",%s", name);
        }
        else
            snprintf(buf, sizeof(buf), "%s", name);
    }

    if (buf[0] == '\0')
        snprintf(active_hook_libs, sizeof(active_hook_libs), "unknown");
    else
        snprintf(active_hook_libs, sizeof(active_hook_libs), "%s", buf);
}

// These contexts are globally known to be long-lived and shared across queries.
// So no wrongful allocations should happen directly in these contexts.
static const char *global_context_names[] = {
    "TopMemoryContext",
    "CacheMemoryContext",
    "ErrorContext",
    NULL
};

bool memcheck_in_internal_query = false;

// Helper function to determine if a context is a known global context or not
// We will use this when we are checking for wrong context allocations.
static bool
is_global_context(const char *name) {
    int i;
    for (i = 0; global_context_names[i] != NULL; i++) {
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
    int i;
    int j;

    // Pass 1: growth in known global contexts
    for (i = 0; i < after->count; i++)
    {
        CtxSnapshot *after_snapshot_entry = &after->entries[i];
        CtxSnapshot *matched_before_snapshot_entry = NULL;

        if (!is_global_context(after_snapshot_entry->name))
            continue;
        for (j = 0; j < before->count; j++)
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
            violation_log_write("wrong_ctx_alloc", "WARNING", detail_msg, active_hook_libs);
        }
    }

    // Pass 2: new contexts created under a global parent
    for (i = 0; i < after->count; i++)
    {
        CtxSnapshot *after_snapshot_entry = &after->entries[i];
        bool is_new = true;
        for (j = 0; j < before->count; j++)
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
            for (j = 0; j < after->count; j++)
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
                    violation_log_write("wrong_ctx_alloc", "WARNING", detail_msg, active_hook_libs);
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
    Size        before_used;
    Size        after_used;
    Size        delta_used;
    const char *severity;
    char        detail[256];

    /*
     * Use net bytes consumed (allocated - free) as the signal rather than
     * raw block-level allocated size.  This catches Bug-2-style leaks where
     * a palloc consumes free space inside an existing large block without
     * requesting a new OS-level block, which leaves totalAllocated unchanged
     * while freespace shrinks permanently.
     *
     * Guard against underflow: freespace should never exceed totalspace, but
     * clamp defensively.
     */
    before_used = (diff->beforeAllocated >= diff->beforeFree)
                  ? diff->beforeAllocated - diff->beforeFree : 0;
    after_used  = (diff->afterAllocated  >= diff->afterFree)
                  ? diff->afterAllocated  - diff->afterFree  : 0;

    if (after_used <= before_used)
        return;

    delta_used = after_used - before_used;

    if (delta_used > (Size)(1 * 1024 * 1024))        /* > 1 MiB */
        severity = "ERROR";
    else if (delta_used > (Size)(64 * 1024))          /* > 64 KiB */
        severity = "WARNING";
    else if (delta_used >= (Size) memcheck_min_leak_bytes)
        severity = "INFO";
    else
        return; /* below the configured threshold, skip silently */

    snprintf(detail, sizeof(detail),
             "context '%s' (depth %d): used grew by %zu bytes "
             "(before=%zu after=%zu); allocated before=%zu after=%zu",
             diff->name, diff->depth,
             delta_used,
             before_used, after_used,
             diff->beforeAllocated, diff->afterAllocated);

    // Finally, write the violation to the shared log
    violation_log_write("context_leak", severity, detail, active_hook_libs);
}

// Install and Uninstall Hooks
void install_executor_hooks(void) {
    // Create the fixed context for before_snapshot allocations
    memcheck_hooks_ctx = AllocSetContextCreate(TopMemoryContext,
                                               "MemCheckHooksContext",
                                               ALLOCSET_DEFAULT_SIZES);

    // Save previous hooks and install our hooks
    prev_executor_start_hook = ExecutorStart_hook;
    ExecutorStart_hook = memcheck_executor_start;

    prev_executor_end_hook = ExecutorEnd_hook;
    ExecutorEnd_hook = memcheck_executor_end;
}

void install_planner_hook(void) {
    prev_planner_hook = planner_hook;
    planner_hook = memcheck_planner_hook;
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
    {
        MemoryContext old_ctx;

        resolve_active_hook_libs();
        old_ctx = MemoryContextSwitchTo(memcheck_hooks_ctx);
        before_snapshot = snapshot_context_tree(TopMemoryContext);
        MemoryContextSwitchTo(old_ctx);
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
    
    // If mode is NONE, do not analyze — but still free any snapshot that was taken
    // at the start of this query (e.g., end() set mode=NONE mid-execution).
    // Leaving before_snapshot non-NULL would create a dangling pointer once the
    // query's memory context is reset, corrupting the heap on the next access.
    if (memcheck_mode == MEMCHECK_NONE) {
        if (before_snapshot != NULL) {
            free_context_tree(before_snapshot);
            before_snapshot = NULL;
        }
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
        int      i;

        // Compute the differences between before and after snapshots
        diffs = diff_context_trees(before_snapshot, after, &diff_count);

        /* Analyze each diff entry and write qualifying violations to shared memory */
        for (i = 0; i < diff_count; i++)
            analyze_and_log_diff(&diffs[i]);

        // Check for wrong context allocations in known global contexts and new contexts created under global parents, 
        // which are common patterns of wrong context usage that can lead to memory leaks across queries.
        check_wrong_context_alloc(before_snapshot, after);

        /* Check for DSM segments attached but not yet detached by this backend */
        dsm_tracker_check_leaks();

        // Clean up snapshots and diffs to avoid memory leaks in the extension itself
        free_context_tree(before_snapshot);
        free_context_tree(after);
        free_context_diff(diffs);
        before_snapshot = NULL;
        active_hook_libs[0] = '\0';
    }
}

// Memcheck Planner Hook Implementation
PlannedStmt *memcheck_planner_hook(Query *parse, const char *query_string, int cursorOptions, ParamListInfo boundParams) {
    // In ALL mode, Memory contexts allocated during planning are also tracked.
    // So we take a before-snapshot at the start of the planner hook, 
    // and the after-snapshot and diff analysis will be done in the ExecutorEnd hook.
    // Means ALL = PLANNER + EXECUTOR, while EXECUTOR = only Executor hooks.
    if (memcheck_mode == MEMCHECK_ALL && !memcheck_in_internal_query && before_snapshot == NULL) {
        MemoryContext old_ctx;

        resolve_active_hook_libs();
        old_ctx = MemoryContextSwitchTo(memcheck_hooks_ctx);
        before_snapshot = snapshot_context_tree(TopMemoryContext);
        MemoryContextSwitchTo(old_ctx);
    }

    if (prev_planner_hook)
        return prev_planner_hook(parse, query_string, cursorOptions, boundParams);
    else
        return standard_planner(parse, query_string, cursorOptions, boundParams);
}