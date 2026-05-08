#ifndef MEMCHECK_HOOKS_H
#define MEMCHECK_HOOKS_H

#include "postgres.h"
#include "fmgr.h"
#include "executor/executor.h"

extern void install_executor_hooks(void);
extern void uninstall_executor_hooks(void);
extern void memcheck_executor_start(QueryDesc *queryDesc, int eflags);
extern void memcheck_executor_end(QueryDesc *queryDesc);

#endif /* MEMCHECK_HOOKS_H */