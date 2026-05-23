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
    Implementation of SQL API functions for pg_ext_memcheck extension.
    This file contains the implementation of the SQL-callable functions for the pg_ext_memcheck extension.

    Functions implemented here include:
    - memcheck_begin 
    - memcheck_end
    - memcheck_run_scenario
*/

// Postgres Includes
#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/timestamp.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"

// Storage Includes
#include "storage/dsm.h"

// Local Includes
#include "include/pg_ext_memcheck.h"
#include "include/gucs.h"
#include "include/violation_log.h"
#include "include/context_walker.h"
#include "include/memcheck_hooks.h"
#include "include/dsm_tracker.h"
#include "include/shmem_probe.h"

#define MAX_CHECKPOINTS 8 

typedef struct BloatSeries {
    char    name[NAMEDATALEN];
    int     depth;
    uint32  parentHash;
    Size    used[MAX_CHECKPOINTS];  /* allocated-free at each checkpoint */
    int   n;                        /* checkpoints recorded */
} BloatSeries;


// Static function declarations
static void run_growth_benchmark(int iterations, const char *workload);
static void run_tx_abort_loop(int iterations, const char *workload);
static void run_shmem_sentinel_probe(int iterations, const char *workload);
static void run_wrong_context_probe(int iterations, const char *workload);
static Size ctx_used_bytes(const CtxSnapshot *s);
static int build_checkpoints(int iterations, int *ckpts);
static void record_checkpoint(BloatSeries **series, int *count, int *cap,
                int ckpt_idx, MemoryContext bench_ctx);

/*
 * memcheck_session_start -- backend-local timestamp set by memcheck_begin().
 *
 * Used by memcheck_end() to drain only violations that belong to the current
 * session (backend_pid == MyProcPid AND ts >= memcheck_session_start).
 * DT_NOBEGIN means no session is active.
 */
static TimestampTz memcheck_session_start = DT_NOBEGIN;
static void analyze_bloat(BloatSeries *series, int count, const int *ckpts);

/* Helper function to calculate used bytes from a context snapshot. */
static Size
ctx_used_bytes(const CtxSnapshot *s)
{
    return (s->totalAllocated >= s->totalFree)
           ? s->totalAllocated - s->totalFree : 0;
}

/* Log-spaced checkpoints (1,10,100,...) capped at iterations, plus iterations itself. */
static int
build_checkpoints(int iterations, int *ckpts)
{
    int  n = 0;
    long p = 1;

    while (p <= iterations && n < MAX_CHECKPOINTS - 1) {
        ckpts[n++] = (int) p;
        p *= 10;
    }
    if (n == 0 || ckpts[n - 1] != iterations) {
        if (n < MAX_CHECKPOINTS) 
            ckpts[n++] = iterations;
        else                     
            ckpts[n - 1] = iterations;
    }
    return n;
}

/*
    * Record a checkpoint of context usage for all contexts in the tree rooted at bench_ctx.
    * This function updates the provided series array with the used bytes for each context at this checkpoint.
    * It also carries forward contexts that disappeared since the last checkpoint, keeping lengths aligned.
*/
static void
record_checkpoint(BloatSeries **series, int *count, int *cap,
                  int ckpt_idx, MemoryContext bench_ctx)
{
    MemoryContext old = MemoryContextSwitchTo(bench_ctx);
    CtxTree *tree     = snapshot_context_tree(TopMemoryContext);
    int      base     = *count;
    bool    *matched  = (bool *) palloc0(sizeof(bool) * (base > 0 ? base : 1));
    int      i, j;

    for (i = 0; i < tree->count; i++) {
        CtxSnapshot *e = &tree->entries[i];
        Size used;
        int  found = -1;

        if (strcmp(e->name, "pg_ext_memcheck bench") == 0)
            continue;                       /* exclude our own bookkeeping ctx */

        used = ctx_used_bytes(e);

        for (j = 0; j < base; j++) {
            BloatSeries *s = &(*series)[j];
            if (!matched[j] && s->depth == e->depth &&
                s->parentHash == e->parentHash &&
                strcmp(s->name, e->name) == 0) { found = j; break; }
        }

        if (found >= 0) {
            BloatSeries *s = &(*series)[found];
            matched[found] = true;
            if (s->n < MAX_CHECKPOINTS)
                s->used[s->n++] = used;
        } else {
            BloatSeries *s;
            if (*count >= *cap) {
                *cap = (*cap == 0) ? 64 : *cap * 2;
                *series = (BloatSeries *) repalloc(*series, sizeof(BloatSeries) * (*cap));
            }
            s = &(*series)[(*count)++];
            snprintf(s->name, NAMEDATALEN, "%s", e->name);
            s->depth = e->depth; s->parentHash = e->parentHash; s->n = 0;
            while (s->n < ckpt_idx && s->n < MAX_CHECKPOINTS) s->used[s->n++] = 0;   /* back-fill late arrivals */
            if (s->n < MAX_CHECKPOINTS)
                s->used[s->n++] = used;
        }
    }

    /* carry-forward series that vanished this checkpoint, keeping lengths aligned */
    for (j = 0; j < base; j++) {
        BloatSeries *s = &(*series)[j];
        if (!matched[j] && s->n <= ckpt_idx) {
            Size carry = (s->n > 0) ? s->used[s->n - 1] : 0;
            while (s->n <= ckpt_idx && s->n < MAX_CHECKPOINTS) s->used[s->n++] = carry;
        }
    }

    pfree(matched);
    free_context_tree(tree);            /* never persist a CtxTree across checkpoints */
    MemoryContextSwitchTo(old);
}

/*
    Analyze the recorded bloat series and log any contexts that show steady growth patterns indicative of bloat.
    This function applies heuristics to determine if a context is bloating and logs violations accordingly.
*/
static void
analyze_bloat(BloatSeries *series, int count, const int *ckpts)
{
    int i, k;

    for (i = 0; i < count; i++) {
        BloatSeries *s = &series[i];
        Size first, last, total_growth;
        bool monotonic = true;
        int  grew = 0;
        double early_rate, late_rate;
        const char *shape, *severity;
        char detail[256];

        if (s->n < 2) continue;
        first = s->used[0];
        last  = s->used[s->n - 1];
        if (last <= first) continue;
        total_growth = last - first;
        if (total_growth < (Size) memcheck_bloat_min_bytes) continue;

        for (k = 1; k < s->n; k++) {
            if (s->used[k] < s->used[k - 1]) { monotonic = false; break; }
            if (s->used[k] > s->used[k - 1]) grew++;
        }
        if (!monotonic || grew < 2) continue;   /* one-shot / noisy, not steady bloat */

        early_rate = (double)(s->used[1] - s->used[0]) / (double)(ckpts[1] - ckpts[0]);
        late_rate  = (double)(s->used[s->n - 1] - s->used[s->n - 2]) /
                     (double)(ckpts[s->n - 1] - ckpts[s->n - 2]);
        shape = (late_rate > 1.5 * early_rate) ? "superlinear" : "linear";

        if      (total_growth > (Size)(1 * 1024 * 1024)) severity = "ERROR";
        else if (total_growth > (Size)(64 * 1024))       severity = "WARNING";
        else                                             severity = "INFO";

        if (strcmp(shape, "superlinear") == 0) {        /* bump one rung */
            if      (strcmp(severity, "INFO") == 0)    severity = "WARNING";
            else if (strcmp(severity, "WARNING") == 0) severity = "ERROR";
        }

        snprintf(detail, sizeof(detail),
                 "context '%s' (depth %d): %s bloat over %d iters, "
                 "used %zu->%zu bytes (+%zu); rate early=%.1f late=%.1f B/iter",
                 s->name, s->depth, shape, ckpts[s->n - 1],
                 first, last, total_growth, early_rate, late_rate);

        violation_log_write("ctx_bloat", severity, detail, active_hook_libs);
    }
}

/*
 * memcheck_begin -- SQL-callable function to start a memory check session.
 *
 * This function sets the memcheck_mode based on the provided argument and initializes any necessary state.
 */
PG_FUNCTION_INFO_V1(memcheck_begin);
Datum
memcheck_begin(PG_FUNCTION_ARGS)
{
    text *mode_text = PG_GETARG_TEXT_PP(0);
    char *mode_str = text_to_cstring(mode_text);

    if (strcmp(mode_str, "executor") == 0)
        memcheck_mode = MEMCHECK_EXECUTOR;
    else if (strcmp(mode_str, "none") == 0)
        memcheck_mode = MEMCHECK_NONE;
    else
        memcheck_mode = MEMCHECK_ALL; // Default to ALL if unrecognized

    /* Record the session start time for scoped draining in memcheck_end(). */
    memcheck_session_start = GetCurrentTimestamp();

     elog(INFO, "Memory check session started with mode: %s", mode_str);

     PG_RETURN_TEXT_P(cstring_to_text("Memory check session started."));
}

/*
 * memcheck_end -- SQL-callable function to end a memory check session.
 *
 * This function resets the memcheck_mode to MEMCHECK_NONE and performs any necessary cleanup.
 * and returns the results of the memory check session, such as any logged violations in the 
 * format check_type TEXT, severity TEXT, detail TEXT, ts TIMESTAMPTZ by reading from the shared violation log.
 * 
 */
PG_FUNCTION_INFO_V1(memcheck_end);
Datum
memcheck_end(PG_FUNCTION_ARGS)
{
    ReturnSetInfo   *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc        tupdesc;
    Tuplestorestate *tupstore;
    ViolationEntry  *entries;
    int              n_entries = 0;
    int              i;

    memcheck_mode = MEMCHECK_NONE;

    elog(INFO, "Memory check session ended.");

    /* Flush any DSM leaks accumulated during this session into the violation log */
    dsm_tracker_check_leaks();

    /* Verify caller can accept a set result */
    if (!rsinfo || !IsA(rsinfo, ReturnSetInfo) ||
        !(rsinfo->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that cannot accept a set")));

    {
        MemoryContext    oldcontext;
        Datum            values[5];
        bool             nulls[5] = {false, false, false, false, false};

        /* All tuplestore/descriptor allocations must live in per-query memory */
        oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);

        /* Build and bless a tuple descriptor for the result set */
        tupdesc = CreateTemplateTupleDesc(5);
        TupleDescInitEntry(tupdesc, (AttrNumber) 1, "check_type", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 2, "severity", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 3, "detail", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 4, "ts", TIMESTAMPTZOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 5, "source_lib", TEXTOID, -1, 0);
        BlessTupleDesc(tupdesc);

        tupstore = tuplestore_begin_heap(true, false, work_mem);
        rsinfo->returnMode = SFRM_Materialize;
        rsinfo->setResult = tupstore;
        rsinfo->setDesc = tupdesc;

        /*
         * Drain only violations that belong to the current session:
         *   - backend_pid must match MyProcPid (own backend)
         *   - ts must be >= memcheck_session_start (set by memcheck_begin)
         *
         * Matched slots are zeroed from the ring atomically, so a second call
         * to end() or flush_violations() will not see the same rows.
         *
         * Contrast with flush_violations(), which drains the entire ring across
         * all backends and inserts rows into ext_memcheck.violation_log.
         */
        if (memcheck_session_start != DT_NOBEGIN)
        {
            entries = violation_log_read_session(MyProcPid, memcheck_session_start, &n_entries);
            if (entries != NULL)
            {
                for (i = 0; i < n_entries; i++)
                {
                    ViolationEntry *e = &entries[i];

                    values[0] = CStringGetTextDatum(e->check_type);
                    values[1] = CStringGetTextDatum(e->severity);
                    values[2] = CStringGetTextDatum(e->detail);
                    values[3] = TimestampTzGetDatum(e->ts);
                    values[4] = CStringGetTextDatum(e->source_lib);

                    tuplestore_putvalues(tupstore, tupdesc, values, nulls);
                }
                pfree(entries);
            }
        }

        /* Reset so a subsequent end() without begin() returns nothing. */
        memcheck_session_start = DT_NOBEGIN;

        MemoryContextSwitchTo(oldcontext);
    }

    PG_RETURN_NULL();
}


/*
    * memcheck_run_scenario -- SQL-callable function to run a predefined memory check scenario.
    *
    * This function takes a scenario name as input, executes the corresponding test scenario, and returns the results.
    * For example, scenarios could include "context_reset_storm", "tx_abort_loop", "concurrent_backends", etc.
    *
*/
PG_FUNCTION_INFO_V1(memcheck_run_scenario);
Datum
memcheck_run_scenario(PG_FUNCTION_ARGS)
{
    text    *scenario_text = PG_GETARG_TEXT_PP(0);
    char    *scenario_str  = text_to_cstring(scenario_text);
    int      iterations    = PG_GETARG_INT32(1);
    text    *workload_text = PG_GETARG_TEXT_PP(2);
    char    *workload_str  = text_to_cstring(workload_text);
    CtxTree *before_snapshot_tree = NULL;
    CtxTree *after_snapshot_tree;
    CtxDiff *diffs;
    int      diff_count;
    int      i;
    bool     run_leak_diff;   /* analyze_and_log_diff -> context_leak */
    bool     run_wrong_ctx;   /* check_wrong_context_alloc -> wrong_ctx_alloc */
    bool     need_snapshots;

    elog(INFO, "Running memory check scenario: %s", scenario_str);
    elog(INFO, "Iterations: %d", iterations);
    elog(INFO, "Workload: %s", workload_str);

    /*
     * Each scenario selects exactly which generic checks run after the workload:
     *   growth_benchmark    -> ctx_bloat only (owns its own checkpointed analysis)
     *   wrong_context_probe -> wrong_ctx_alloc only
     *   everything else     -> context_leak + wrong_ctx_alloc
     */
    if (strcmp(scenario_str, "growth_benchmark") == 0) {
        run_leak_diff = false;
        run_wrong_ctx = false;
    } else if (strcmp(scenario_str, "wrong_context_probe") == 0) {
        run_leak_diff = false;
        run_wrong_ctx = true;
    } else {
        run_leak_diff = true;
        run_wrong_ctx = true;
    }
    need_snapshots = run_leak_diff || run_wrong_ctx;

    if (need_snapshots)
        before_snapshot_tree = snapshot_context_tree(TopMemoryContext);

    memcheck_in_internal_query = true;

    PG_TRY(); 
    {
        if (strcmp(scenario_str, "growth_benchmark") == 0) {
            run_growth_benchmark(iterations, workload_str);
        } else if (strcmp(scenario_str, "tx_abort_loop") == 0) {
            run_tx_abort_loop(iterations, workload_str);
        } else if (strcmp(scenario_str, "shmem_sentinel_probe") == 0) {
            run_shmem_sentinel_probe(iterations, workload_str);
        } else if (strcmp(scenario_str, "wrong_context_probe") == 0) {
            run_wrong_context_probe(iterations, workload_str);
        } else {
            memcheck_in_internal_query = false;
            elog(ERROR, "Unknown scenario: %s", scenario_str);
        }
    } 
    PG_CATCH();
    {
        memcheck_in_internal_query = false;
        PG_RE_THROW();
    }
    PG_END_TRY();

    memcheck_in_internal_query = false;

    if (need_snapshots) {
        after_snapshot_tree = snapshot_context_tree(TopMemoryContext);

        if (run_leak_diff) {
            diff_count = 0;
            diffs = diff_context_trees(before_snapshot_tree, after_snapshot_tree, &diff_count);
            for (i = 0; i < diff_count; i++) {
                analyze_and_log_diff(&diffs[i]);
            }
        }

        if (run_wrong_ctx)
            check_wrong_context_alloc(before_snapshot_tree, after_snapshot_tree);
    }

    PG_RETURN_TEXT_P(cstring_to_text("Scenario executed and analyzed. Run 'SELECT * FROM ext_memcheck.end()' to retrieve logged violations."));
}

static void
run_shmem_sentinel_probe(int iterations, const char *workload)
{
    int i;

    if (iterations <= 0)
        elog(ERROR, "Iterations must be a positive integer");

    /* Register sentinels for our own shmem segments (allocated with +1 byte). */
    probe_register("pg_ext_memcheck ViolationLog", sizeof(ViolationLog) + 1, sizeof(ViolationLog)); /* sentinel at the end of the struct */
    probe_register("pg_ext_memcheck DsmTrackerState", sizeof(DsmTrackerState) + 1, sizeof(DsmTrackerState)); /* sentinel at the end of the struct */

    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "pg_ext_memcheck: SPI_connect failed");

    for (i = 0; i < iterations; i++)
    {
        int ret = SPI_execute(workload, true, 0);
        if (ret != SPI_OK_SELECT)
            elog(ERROR, "pg_ext_memcheck: SPI_execute failed with code %d", ret);
    }

    SPI_finish();

    /* Check all sentinels after the workload. */
    probe_check_all();
}

static void
run_growth_benchmark(int iterations, const char *workload)
{
    int            ckpts[MAX_CHECKPOINTS];
    int            nckpt, ckpt_idx, i;
    MemoryContext  bench_ctx, old;
    BloatSeries   *series = NULL;
    int            series_count = 0, series_cap = 64;

    if (iterations <= 0)
        elog(ERROR, "Iterations must be a positive integer");

    nckpt     = build_checkpoints(iterations, ckpts);
    bench_ctx = AllocSetContextCreate(TopMemoryContext, "pg_ext_memcheck bench",
                                      ALLOCSET_DEFAULT_SIZES);

    if (SPI_connect() != SPI_OK_CONNECT) {
        MemoryContextDelete(bench_ctx);
        elog(ERROR, "pg_ext_memcheck: SPI_connect failed");
    }

    old = MemoryContextSwitchTo(bench_ctx);
    series = (BloatSeries *) palloc(sizeof(BloatSeries) * series_cap);
    MemoryContextSwitchTo(old);

    ckpt_idx = 0;
    for (i = 1; i <= iterations; i++) {
        int ret = SPI_execute(workload, true, 0);
        if (ret != SPI_OK_SELECT) {
            SPI_finish();
            MemoryContextDelete(bench_ctx);
            elog(ERROR, "pg_ext_memcheck: SPI_execute failed with code %d", ret);
        }
        if (ckpt_idx < nckpt && i == ckpts[ckpt_idx]) {   /* snapshot between statements */
            record_checkpoint(&series, &series_count, &series_cap, ckpt_idx, bench_ctx);
            ckpt_idx++;
        }
    }

    SPI_finish();
    analyze_bloat(series, series_count, ckpts);
    MemoryContextDelete(bench_ctx);
}

static void
run_tx_abort_loop(int iterations, const char *workload) {
    int i;
    if (iterations <= 0)
        elog(ERROR, "Iterations must be a positive integer");

    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "pg_ext_memcheck: SPI_connect failed");

    for (i = 0; i < iterations; i++) {
        int ret = SPI_execute("SAVEPOINT _memcheck_sp", false, 0);
        if (ret != SPI_OK_UTILITY)
            elog(ERROR, "pg_ext_memcheck: SPI_execute failed with code %d", ret);

        ret = SPI_execute(workload, true, 0);
        if (ret != SPI_OK_SELECT)
            elog(ERROR, "pg_ext_memcheck: SPI_execute failed with code %d", ret);

        ret = SPI_execute("ROLLBACK TO SAVEPOINT _memcheck_sp", false, 0);
        if (ret != SPI_OK_UTILITY)
            elog(ERROR, "pg_ext_memcheck: SPI_execute failed with code %d", ret);
    }

    SPI_execute("RELEASE SAVEPOINT _memcheck_sp", false, 0); // Clean up savepoint after loop

    SPI_finish();
}

static void
run_wrong_context_probe(int iterations, const char *workload)
{
    int i;

    if (iterations <= 0)
        elog(ERROR, "Iterations must be a positive integer");

    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "pg_ext_memcheck: SPI_connect failed");

    for (i = 0; i < iterations; i++) {
        int ret = SPI_execute(workload, true, 0);
        if (ret != SPI_OK_SELECT)
            elog(ERROR, "pg_ext_memcheck: SPI_execute failed with code %d", ret);
    }

    SPI_finish();
    /* Detection happens in memcheck_run_scenario via check_wrong_context_alloc. */
}

PG_FUNCTION_INFO_V1(dsm_tracker_list_segments);
Datum
dsm_tracker_list_segments(PG_FUNCTION_ARGS)
{
    ReturnSetInfo   *rsinfo;
    TupleDesc        tupdesc;
    Tuplestorestate *tupstore;
    int              i;

    if (dsm_tracker_state == NULL)
        PG_RETURN_NULL();

    rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

    /* Verify caller can accept a set result */
    if (!rsinfo || !IsA(rsinfo, ReturnSetInfo) ||
        !(rsinfo->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that cannot accept a set")));

    {
        MemoryContext    oldcontext;
        Datum            values[5];
        bool             nulls[5] = {false, false, false, false, false};

        /* All tuplestore/descriptor allocations must live in per-query memory */
        oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);

        /* Build and bless a tuple descriptor for the result set */
        tupdesc = CreateTemplateTupleDesc(5);
        TupleDescInitEntry(tupdesc, (AttrNumber) 1, "handle",       INT8OID,        -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 2, "backend_pid",  INT4OID,        -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 3, "attached_at",  TIMESTAMPTZOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 4, "size_bytes",   INT8OID,        -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 5, "detached",     BOOLOID,        -1, 0);
        BlessTupleDesc(tupdesc);

        tupstore = tuplestore_begin_heap(true, false, work_mem);
        rsinfo->returnMode = SFRM_Materialize;
        rsinfo->setResult  = tupstore;
        rsinfo->setDesc    = tupdesc;

        for (i = 0; i < dsm_tracker_state->count; i++)
        {
            DsmSegmentRecord *r = &dsm_tracker_state->segments[i];
            bool live_detached = r->detached;

            /*
             * For segments we observed externally (via track_dsm_handle) we
             * never hold a live attachment and cannot rely on the detach
             * callback.  Probe the handle right now: if dsm_attach succeeds
             * the segment still exists → still active; if it returns NULL the
             * segment has been destroyed → treat as detached.  Immediately
             * detach our probe attachment so we leave no resource open.
             */
            if (!r->detached)
            {
                dsm_segment *probe = NULL;
                bool attach_failed = false;

                /*
                 * dsm_attach can ereport(WARNING) and return NULL, or in some
                 * cases throw a recoverable error, when the segment no longer
                 * exists.  Catch any such error so a destroyed-but-untracked
                 * segment doesn't abort the whole query.
                 */
                PG_TRY();
                {
                    probe = dsm_attach(r->handle);
                }
                PG_CATCH();
                {
                    FlushErrorState();
                    attach_failed = true;
                }
                PG_END_TRY();

                if (probe == NULL || attach_failed)
                {
                    live_detached = true;
                    /* Persist so future calls skip the probe */
                    LWLockAcquire(&dsm_tracker_state->lock, LW_EXCLUSIVE);
                    r->detached = true;
                    LWLockRelease(&dsm_tracker_state->lock);
                }
                else
                    dsm_detach(probe);
            }

            values[0] = Int64GetDatum((int64) r->handle);
            values[1] = Int32GetDatum(r->backend_pid);
            values[2] = TimestampTzGetDatum(r->attached_at);
            values[3] = Int64GetDatum((int64) r->size_bytes);
            values[4] = BoolGetDatum(live_detached);

            tuplestore_putvalues(tupstore, tupdesc, values, nulls);
        }

        MemoryContextSwitchTo(oldcontext);
    }

    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(dsm_tracker_handle);
Datum
dsm_tracker_handle(PG_FUNCTION_ARGS)
{
    int64        handle_arg = PG_GETARG_INT64(0);
    dsm_handle   handle;
    dsm_segment *seg;
    Size         seg_size;

    if (handle_arg < 0 || handle_arg > (int64) UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("handle %lld is out of range for a DSM handle (0..%u)",
                        (long long) handle_arg, UINT32_MAX)));

    handle = (dsm_handle) handle_arg;

    if (dsm_tracker_state == NULL)
        PG_RETURN_NULL();

    seg = dsm_attach(handle);
    if (seg == NULL)
        elog(ERROR, "Failed to attach to DSM segment with handle %u", handle);

    seg_size = dsm_segment_map_length(seg);

    /*
     * Record the handle as observed (not-detached) WITHOUT registering an
     * on_dsm_detach callback.  We detach our own reference right away so
     * PostgreSQL does not emit "resource was not closed".  The record stays
     * detached=false until the user calls clear_dsm_tracking() or end().
     */
    dsm_tracker_record_handle_observe(handle, seg_size);
    dsm_detach(seg);

    elog(INFO, "Attached to DSM segment with handle %u, size %zu bytes", handle, seg_size);

    PG_RETURN_TEXT_P(cstring_to_text("DSM segment tracked."));
}

PG_FUNCTION_INFO_V1(clear_dsm_tracking);
Datum
clear_dsm_tracking(PG_FUNCTION_ARGS)
{
    if (dsm_tracker_state == NULL)
        PG_RETURN_NULL(); /* tracker not initialized, should not happen but be defensive */
    
    LWLockAcquire(&dsm_tracker_state->lock, LW_EXCLUSIVE);
    dsm_tracker_state->count = 0; 
    LWLockRelease(&dsm_tracker_state->lock);    
    elog(INFO, "Cleared DSM tracking records in pg_ext_memcheck");
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(shmem_probe_register);
Datum 
shmem_probe_register(PG_FUNCTION_ARGS)
{
    text *name_text = PG_GETARG_TEXT_PP(0);
    char *name_str  = text_to_cstring(name_text);
    Size size_bytes = (Size) PG_GETARG_INT64(1);

    probe_register(name_str, size_bytes, size_bytes); /* sentinel at the end of the segment */
    elog(INFO, "Registered shmem probe '%s' with size %zu bytes", name_str, size_bytes);
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(shmem_probe_clear_registry);
Datum
shmem_probe_clear_registry(PG_FUNCTION_ARGS)
{
    if (probe_registry == NULL)
        PG_RETURN_VOID();

    probe_registry_clear();
    elog(INFO, "Cleared shmem probe registry in pg_ext_memcheck");
    PG_RETURN_VOID();
}