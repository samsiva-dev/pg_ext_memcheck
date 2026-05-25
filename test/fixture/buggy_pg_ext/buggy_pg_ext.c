/*
 * buggy_pg_ext.c
 *
 * An intentionally buggy PostgreSQL extension for memory-leak testing.
 * Source: https://github.com/samsiva-dev/buggy-pg-ext
 *
 * BUG 1 (Planner hook):
 *   Creates a new child MemoryContext under MessageContext on every query and
 *   allocates 8192 bytes into it.  The context is never reset or deleted, so
 *   the allocation lives until MessageContext itself is eventually reset.
 *   In a long-running session with many queries this accumulates significantly.
 *
 * BUG 2 (ExecutorStart hook):
 *   Allocates 8192 bytes directly inside TopMemoryContext on every query.
 *   TopMemoryContext is never reset during normal operation, so each
 *   allocation leaks permanently for the lifetime of the backend process.
 *
 * BUG 3 (ExecutorStart hook – DSM leak):
 *   Creates a 64 KB Dynamic Shared Memory (DSM) segment on every query via
 *   dsm_create(), then calls dsm_pin_segment() to keep it alive beyond the
 *   current backend, and immediately loses the handle without ever calling
 *   dsm_detach().
 *
 * Load via postgresql.conf:
 *   shared_preload_libraries = 'buggy_pg_ext'
 */

#include "postgres.h"

#include "executor/executor.h"
#include "optimizer/planner.h"
#include "storage/dsm.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"

#include <string.h>

PG_MODULE_MAGIC;

/* --------------------------------------------------------------------------
 * Saved previous hook pointers so we form a proper chain.
 * -------------------------------------------------------------------------- */
static planner_hook_type       prev_planner_hook       = NULL;
static ExecutorStart_hook_type prev_ExecutorStart_hook = NULL;

/*
 * Last leaked DSM handle – written by buggy_ExecutorStart() every time it
 * creates a new segment, readable via the SQL function
 * buggy_last_dsm_handle() so callers can pass it to their tracking harness.
 */
static dsm_handle last_leaked_dsm_handle = DSM_HANDLE_INVALID;

/* --------------------------------------------------------------------------
 * Forward declarations
 * -------------------------------------------------------------------------- */
static PlannedStmt *buggy_planner(Query *parse,
                                   const char *query_string,
                                   int cursorOptions,
                                   ParamListInfo boundParams);

static void buggy_ExecutorStart(QueryDesc *queryDesc, int eflags);

/*
 * buggy_last_dsm_handle()
 *
 * SQL-callable accessor that returns the dsm_handle of the most recently
 * created (leaked) DSM segment as int8.
 *
 * Returns 0 (DSM_HANDLE_INVALID) if no segment has been created yet.
 */
PG_FUNCTION_INFO_V1(buggy_last_dsm_handle);
extern Datum buggy_last_dsm_handle(PG_FUNCTION_ARGS);

/* --------------------------------------------------------------------------
 * _PG_init – called when the shared library is loaded
 * -------------------------------------------------------------------------- */
void _PG_init(void)
{
    prev_planner_hook       = planner_hook;
    planner_hook            = buggy_planner;

    prev_ExecutorStart_hook = ExecutorStart_hook;
    ExecutorStart_hook      = buggy_ExecutorStart;
}

/* --------------------------------------------------------------------------
 * _PG_fini – called when the shared library is unloaded
 * -------------------------------------------------------------------------- */
void _PG_fini(void)
{
    planner_hook       = prev_planner_hook;
    ExecutorStart_hook = prev_ExecutorStart_hook;
}

/* --------------------------------------------------------------------------
 * buggy_planner
 *
 * BUG 1: A new child MemoryContext is created under MessageContext for every
 * query.  8192 bytes are allocated inside it.  Neither the allocation nor the
 * context is ever freed / deleted / reset.
 * -------------------------------------------------------------------------- */
static PlannedStmt *
buggy_planner(Query *parse,
              const char *query_string,
              int cursorOptions,
              ParamListInfo boundParams)
{
    MemoryContext leaky_plan_ctx;
    char         *leaked_buf;

    leaky_plan_ctx =
        AllocSetContextCreate(MessageContext,
                              "BuggyPlannerLeakCtx",
                              ALLOCSET_DEFAULT_SIZES);

    /* Allocate 8192 bytes – BUG: never freed, context never deleted. */
    leaked_buf = (char *) MemoryContextAlloc(leaky_plan_ctx, 8192);
    memset(leaked_buf, 0xAB, 8192);

    /*
     * BUG: missing cleanup:
     *   MemoryContextDelete(leaky_plan_ctx);
     */

    /* Chain to the next hook or the built-in planner. */
    if (prev_planner_hook)
        return prev_planner_hook(parse, query_string, cursorOptions,
                                 boundParams);
    else
        return standard_planner(parse, query_string, cursorOptions,
                                boundParams);
}

/* --------------------------------------------------------------------------
 * buggy_ExecutorStart
 *
 * BUG 2: 8192 bytes are palloc'd directly inside TopMemoryContext on every
 * query.  TopMemoryContext is never reset during normal backend operation,
 * so every allocation is a permanent memory leak for the backend process.
 *
 * BUG 3: A 64 KB DSM segment is created and pinned but the handle is
 * immediately abandoned without ever calling dsm_detach().
 * -------------------------------------------------------------------------- */
static void
buggy_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
    char *leaked_exec_buf;

    /*
     * BUG 2: Allocate into TopMemoryContext.
     */
    leaked_exec_buf = (char *) MemoryContextAlloc(TopMemoryContext, 8192);
    memset(leaked_exec_buf, 0xCD, 8192);

    /*
     * BUG: missing cleanup:
     *   pfree(leaked_exec_buf);
     */

    /*
     * BUG 3: DSM segment leak.
     */
    {
        dsm_segment *leaked_dsm_seg;

        leaked_dsm_seg = dsm_create(65536, 0);  /* 64 KB DSM segment */

        last_leaked_dsm_handle = dsm_segment_handle(leaked_dsm_seg);

        dsm_pin_segment(leaked_dsm_seg);         /* BUG: survives session exit */
        /* BUG: handle abandoned – dsm_detach(leaked_dsm_seg) never called */
        (void) leaked_dsm_seg;
    }

    /* Chain to the next hook or the built-in executor start. */
    if (prev_ExecutorStart_hook)
        prev_ExecutorStart_hook(queryDesc, eflags);
    else
        standard_ExecutorStart(queryDesc, eflags);
}

/* --------------------------------------------------------------------------
 * buggy_last_dsm_handle
 *
 * Returns last_leaked_dsm_handle as int8 so it can be passed directly to
 * the tracking harness:
 *
 *   SELECT ext_memcheck.track_dsm_handle(buggy_last_dsm_handle());
 *
 * dsm_handle is typedef'd as uint32.  We widen it to int8 (int64) to avoid
 * sign issues when large handle values are returned to SQL.
 * -------------------------------------------------------------------------- */
Datum
buggy_last_dsm_handle(PG_FUNCTION_ARGS)
{
    PG_RETURN_INT64((int64) last_leaked_dsm_handle);
}
