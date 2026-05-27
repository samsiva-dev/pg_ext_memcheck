/*-------------------------------------------------------------------------
 * pg_ext_memcheck
 *
 * Copyright (c) 2026, Samba Siva Reddy
 *
 * This software is released under the MIT License.
 * See LICENSE for details.
 *-------------------------------------------------------------------------
*/
#ifndef MEMCHECK_HOOKS_H
#define MEMCHECK_HOOKS_H

#include "postgres.h"
#include "fmgr.h"
#include "executor/executor.h"
#include "pg_config_manual.h"

#include "include/context_walker.h"

/* Maximum number of contexts in the per-session allowlist. */
#define MEMCHECK_MAX_ALLOWED_CTXS 16

/*
 * Session-scoped/Test-window targeting state — set by ext_memcheck.begin() and cleared by
 * ext_memcheck.end().  These are consulted by analyze_and_log_diff() and
 * check_wrong_context_alloc() to scope detection to the target extension.
 *
 * ext_context_pattern  : SQL LIKE pattern (% wildcard) for context names to
 *                        monitor.  Empty string means "monitor all contexts".
 * ext_allowed_contexts : Allowlist of context names that are explicitly
 *                        permitted to grow without being flagged as a violation,
 *                        (e.g., "TopMemoryContext" for extensions that
 *                        intentionally cache data across queries).
 * ext_n_allowed_contexts: Number of valid entries in ext_allowed_contexts.
 * ext_track_shmem      : Whether shmem sentinel checks are active.
 * ext_track_dsm        : Whether DSM leak checks are active.
 */
extern char ext_context_pattern[NAMEDATALEN];
extern char ext_allowed_contexts[MEMCHECK_MAX_ALLOWED_CTXS][NAMEDATALEN];
extern int  ext_n_allowed_contexts;
extern bool ext_track_shmem;
extern bool ext_track_dsm;

/* Returns true if 'name' matches ext_context_pattern (or pattern is empty). */
extern bool ctx_matches_target(const char *name);

/* Returns true if 'name' is in the per-session allowlist. */
extern bool is_allowed_context_target(const char *name);

// Executor Hooks
extern void install_executor_hooks(void);
extern void install_planner_hook(void);

// Hook implementations functions
extern void memcheck_executor_start(QueryDesc *queryDesc, int eflags);
extern void memcheck_executor_end(QueryDesc *queryDesc);
extern PlannedStmt *memcheck_planner_hook(Query *parse, const char *query_string, int cursorOptions, ParamListInfo boundParams);

// Helper functions for diff analysis and wrong context allocation checks, called from memcheck_executor_end

void analyze_and_log_diff(CtxDiff *diff);
void check_wrong_context_alloc(CtxTree *before, CtxTree *after);

/*
 * Discard the before-snapshot held by the outer executor/planner hook, if any.
 * Must be called at the entry of SQL functions that perform their own
 * before/after analysis (e.g. memcheck_run_scenario) so that the outer
 * ExecutorEnd hook does not re-report the same memory delta a second time
 * with an empty source_lib.
 */
void memcheck_discard_outer_hook_snapshot(void);

// Global variable to track the active hook libraries for logging purposes, set by resolve_active_hook_libs().
extern char active_hook_libs[128];

#endif /* MEMCHECK_HOOKS_H */