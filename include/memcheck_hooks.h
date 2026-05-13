#ifndef MEMCHECK_HOOKS_H
#define MEMCHECK_HOOKS_H

#include "postgres.h"
#include "fmgr.h"
#include "executor/executor.h"

#include "include/context_walker.h"

// Executor Hooks
extern void install_executor_hooks(void);
extern void uninstall_executor_hooks(void);
extern void install_planner_hook(void);
extern void uninstall_planner_hook(void);

// Hook implementations functions
extern void memcheck_executor_start(QueryDesc *queryDesc, int eflags);
extern void memcheck_executor_end(QueryDesc *queryDesc);
extern PlannedStmt *memcheck_planner_hook(Query *parse, const char *query_string, int cursorOptions, ParamListInfo boundParams);

void analyze_and_log_diff(CtxDiff *diff);
void check_wrong_context_alloc(CtxTree *before, CtxTree *after);

#endif /* MEMCHECK_HOOKS_H */