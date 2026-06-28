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
#include "access/xact.h"      /* BeginInternalSubTransaction, Rollback/ReleaseCurrentSubTransaction */

// JSONB for options parsing
#include "utils/jsonb.h"

// Local Includes
#include "include/pg_ext_memcheck.h"
#include "include/gucs.h"
#include "include/violation_log.h"
#include "include/context_walker.h"
#include "include/memcheck_hooks.h"
#include "include/dsm_tracker.h"
#include "include/shmem_probe.h"
#include "include/worker_harness.h"

#define MAX_CHECKPOINTS 8

/*
 * Per-session start timestamp set by memcheck_begin() and consumed by
 * memcheck_end().  end() passes this to violation_log_read_session() so that
 * only violations logged *during* the current session window are returned,
 * filtering out anything written before begin() was called (by other sessions
 * or by a previous begin()/end() cycle in the same backend).
 *
 * Value 0 (epoch) means no session is active; end() returns an empty set.
 */
static TimestampTz memcheck_session_start = 0;

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
static void run_use_after_reset(int iterations, const char *workload);
static void run_oom_simulation(int iterations, const char *workload);
static void run_context_reset_storm(int iterations, const char *workload);
static void run_cursor_leak(int iterations, const char *workload);
static void run_cold_warm_cold(int iterations, const char *workload);
static void run_concurrent_backends(int iterations, const char *workload);
static Size ctx_used_bytes(const CtxSnapshot *s);
static int build_checkpoints(int iterations, int *ckpts);
static void record_checkpoint(BloatSeries **series, int *count, int *cap,
                int ckpt_idx, MemoryContext bench_ctx);
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
        /* Skip contexts that don't match the active target pattern, consistent
         * with how analyze_and_log_diff scopes context_leak reporting. */
        if (!ctx_matches_target(s->name)) continue;
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
 * Arguments:
 *   ext_context_pattern TEXT   -- SQL LIKE pattern (% wildcard) for context names
 *                                 to monitor, e.g. 'MyExtCtx%'.  Empty string
 *                                 monitors all contexts (pre-scoping behaviour).
 *   options JSONB DEFAULT NULL  -- Optional targeting options:
 *                                  {
 *                                    "track_shmem":      bool (default true),
 *                                    "track_dsm":        bool (default true),
 *                                    "allowed_contexts": ["CtxName", ...]
 *                                  }
 *                                  allowed_contexts is an allowlist: contexts in
 *                                  this list are never flagged even if they grow.
 *
 * Stores the targeting state into module-level variables in memcheck_hooks.c
 * that are consulted by analyze_and_log_diff() and check_wrong_context_alloc()
 * to scope detection to the target extension.  The monitoring mode
 * (all / executor / none) is controlled separately via the
 * pg_ext_memcheck.memcheck_mode GUC.
 */
PG_FUNCTION_INFO_V1(memcheck_begin);
Datum
memcheck_begin(PG_FUNCTION_ARGS)
{
    text    *pattern_text;
    char    *pattern_str;
    int      pattern_len;

    /* First arg: ext_context_pattern (not null; empty string = match all) */
    if (PG_ARGISNULL(0))
        pattern_str = "";
    else
    {
        pattern_text = PG_GETARG_TEXT_PP(0);
        pattern_str  = text_to_cstring(pattern_text);
    }

    pattern_len = strlen(pattern_str);
    if (pattern_len >= NAMEDATALEN)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("ext_context_pattern too long (max %d characters)",
                        NAMEDATALEN - 1)));

    strncpy(ext_context_pattern, pattern_str, NAMEDATALEN);
    ext_context_pattern[NAMEDATALEN - 1] = '\0';

    /* Reset options to defaults before applying any caller-supplied values */
    ext_track_shmem        = true;
    ext_track_dsm          = true;
    ext_n_allowed_contexts = 0;

    /* Second arg: options JSONB (optional — may be NULL or omitted) */
    if (!PG_ARGISNULL(1))
    {
        Jsonb              *opts = PG_GETARG_JSONB_P(1);
        JsonbIterator      *it;
        JsonbValue          v;
        JsonbIteratorToken  tok;
        char                cur_key[64] = "";
        bool                in_allowed_array = false;

        it = JsonbIteratorInit(&opts->root);
        while ((tok = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
        {
            switch (tok)
            {
                case WJB_KEY:
                {
                    int klen = Min((int) v.val.string.len,
                                   (int) sizeof(cur_key) - 1);
                    memcpy(cur_key, v.val.string.val, klen);
                    cur_key[klen]    = '\0';
                    in_allowed_array = false;
                    break;
                }
                case WJB_VALUE:
                    if (strcmp(cur_key, "track_shmem") == 0 &&
                        v.type == jbvBool)
                        ext_track_shmem = v.val.boolean;
                    else if (strcmp(cur_key, "track_dsm") == 0 &&
                             v.type == jbvBool)
                        ext_track_dsm = v.val.boolean;
                    break;
                case WJB_BEGIN_ARRAY:
                    if (strcmp(cur_key, "allowed_contexts") == 0)
                        in_allowed_array = true;
                    break;
                case WJB_END_ARRAY:
                    in_allowed_array = false;
                    break;
                case WJB_ELEM:
                    if (in_allowed_array && v.type == jbvString &&
                        ext_n_allowed_contexts < MEMCHECK_MAX_ALLOWED_CTXS)
                    {
                        int elen = Min((int) v.val.string.len,
                                       NAMEDATALEN - 1);
                        memcpy(ext_allowed_contexts[ext_n_allowed_contexts],
                               v.val.string.val, elen);
                        ext_allowed_contexts[ext_n_allowed_contexts][elen] = '\0';
                        ext_n_allowed_contexts++;
                    }
                    break;
                default:
                    break;
            }
        }
    }

    /*
     * Activate monitoring.  If the user pre-SET memcheck_mode to 'executor'
     * or 'all', honour that choice.  If the mode is still NONE (the default,
     * or left over after a previous end()), begin() activates MEMCHECK_ALL so
     * that calling begin() without any prior SET "just works".
     *
     * This makes begin()/end() symmetric: begin opens the window (monitoring
     * on), end() closes it (monitoring off via MEMCHECK_NONE).  The GUC
     * continues to function as an explicit override when the user wants a
     * specific level before calling begin().
     */
    if (memcheck_mode == MEMCHECK_NONE)
        memcheck_mode = MEMCHECK_ALL;

    /* Record the window start time before logging so the INFO message itself
     * (which may trigger hook re-entry in ALL mode) is excluded from this
     * session's results. */
    memcheck_session_start = GetCurrentTimestamp();

    elog(INFO, "Memory check session started: pattern='%s', allowed_contexts=%d",
         ext_context_pattern[0] ? ext_context_pattern : "(all)",
         ext_n_allowed_contexts);

    PG_RETURN_TEXT_P(cstring_to_text("Memory check session started."));
}

/*
 * memcheck_end -- SQL-callable function to end a memory check session.
 *
 * Closes the current test window.  Returns the subset of ring-buffer entries
 * that belong to this session:
 *   backend_pid == MyProcPid  AND  ts >= memcheck_session_start
 *
 * Matched entries are atomically zeroed from the ring buffer so that a second
 * call to end() returns 0 rows, and flush_violations() never re-surfaces them.
 * If begin() was not called (memcheck_session_start == 0) an empty set is
 * returned.
 */
PG_FUNCTION_INFO_V1(memcheck_end);
Datum
memcheck_end(PG_FUNCTION_ARGS)
{
    ReturnSetInfo   *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc        tupdesc;
    Tuplestorestate *tupstore;
    ViolationEntry  *entries = NULL;
    int              n_entries = 0;
    int              i;
    TimestampTz      session_start;

    memcheck_mode = MEMCHECK_NONE;

    /* Snapshot and reset the session start time before any further work so
     * that violations written by dsm_tracker_check_leaks() (called below)
     * are included in this window if they arrive after we save the value. */
    session_start         = memcheck_session_start;
    memcheck_session_start = 0;

    /* Capture track_dsm before clearing state so the guard below sees the
     * value set by begin(), not the post-reset default. */
    {
        bool save_track_dsm = ext_track_dsm;

        /* Clear per-session targeting state so the next begin() starts clean */
        ext_context_pattern[0] = '\0';
        ext_n_allowed_contexts = 0;
        ext_track_shmem        = true;
        ext_track_dsm          = true;

        elog(INFO, "Memory check session ended.");

        /* Flush any DSM leaks accumulated during this session into the violation log */
        if (save_track_dsm)
            dsm_tracker_check_leaks();
    }

    /* Verify caller can accept a set result */
    if (!rsinfo || !IsA(rsinfo, ReturnSetInfo) ||
        !(rsinfo->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that cannot accept a set")));

    /*
     * Drain only this session's entries from the shared ring buffer.
     * session_start == 0 means begin() was never called; return empty set.
     */
    if (session_start != 0)
        entries = violation_log_read_session(MyProcPid, session_start, &n_entries);

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
    int      iterations    = PG_NARGS() > 1 ? PG_GETARG_INT32(1) : 100;
    text    *workload_text = PG_NARGS() > 2 ? PG_GETARG_TEXT_PP(2) : NULL;
    char    *workload_str  = workload_text ? text_to_cstring(workload_text) : "SELECT 1";
    CtxTree *before_snapshot_tree = NULL;
    CtxTree *after_snapshot_tree  = NULL;
    CtxDiff *diffs                = NULL;
    int      diff_count;
    int      i;
    bool     run_leak_diff;   /* analyze_and_log_diff -> context_leak */
    bool     run_wrong_ctx;   /* check_wrong_context_alloc -> wrong_ctx_alloc */
    bool     need_snapshots;

    /*
     * The planner/executor hooks already took a before-snapshot for this SQL
     * function call.  If we leave it in place, ExecutorEnd will fire after we
     * return and re-report every violation we generate here — with an empty
     * source_lib — producing exact duplicates.  Discard it now so ExecutorEnd
     * finds before_snapshot == NULL and skips the outer analysis entirely.
     */
    memcheck_discard_outer_hook_snapshot();

    elog(INFO, "Running memory check scenario: %s", scenario_str);
    elog(INFO, "Iterations: %d", iterations);
    elog(INFO, "Workload: %s", workload_str);

    /*
     * Each scenario selects exactly which generic checks run after the workload:
     *   growth_benchmark        -> ctx_bloat only (owns its own checkpointed analysis)
     *   wrong_context_probe     -> wrong_ctx_alloc only
     *   use_after_reset         -> none (BGWorker handles internally)
     *   oom_simulation          -> none (BGWorker handles internally)
     *   concurrent_backends     -> none (BGWorker handles internally per-worker)
     *   cursor_leak             -> context_leak only (no wrong-ctx expected)
     *   everything else         -> context_leak + wrong_ctx_alloc
     */
    if (strcmp(scenario_str, "growth_benchmark") == 0) {
        run_leak_diff = false;
        run_wrong_ctx = false;
    } else if (strcmp(scenario_str, "wrong_context_probe") == 0) {
        run_leak_diff = false;
        run_wrong_ctx = true;
    } else if (strcmp(scenario_str, "use_after_reset") == 0 ||
               strcmp(scenario_str, "oom_simulation") == 0 ||
               strcmp(scenario_str, "concurrent_backends") == 0) {
        run_leak_diff = false;
        run_wrong_ctx = false;
    } else if (strcmp(scenario_str, "cursor_leak") == 0) {
        run_leak_diff = true;
        run_wrong_ctx = false;
    } else {
        run_leak_diff = true;
        run_wrong_ctx = true;
    }
    need_snapshots = run_leak_diff || run_wrong_ctx;

    if (need_snapshots)
        before_snapshot_tree = snapshot_context_tree(TopMemoryContext);

    /*
     * Scenario violations are generated outside the executor-hook path, so
     * active_hook_libs is empty.  Stamp a deterministic marker so every
     * violation produced by this scenario (analyze_and_log_diff,
     * check_wrong_context_alloc, analyze_bloat, probe_check_all) carries
     * meaningful attribution in ext_memcheck.violation_log.source_lib.
     */
    snprintf(active_hook_libs, sizeof(active_hook_libs), "(scenario:%s)", scenario_str);

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
        } else if (strcmp(scenario_str, "use_after_reset") == 0) {
            run_use_after_reset(iterations, workload_str);
        } else if (strcmp(scenario_str, "oom_simulation") == 0) {
            run_oom_simulation(iterations, workload_str);
        } else if (strcmp(scenario_str, "context_reset_storm") == 0) {
            run_context_reset_storm(iterations, workload_str);
        } else if (strcmp(scenario_str, "cursor_leak") == 0) {
            run_cursor_leak(iterations, workload_str);
        } else if (strcmp(scenario_str, "cold_warm_cold") == 0) {
            run_cold_warm_cold(iterations, workload_str);
        } else if (strcmp(scenario_str, "concurrent_backends") == 0) {
            run_concurrent_backends(iterations, workload_str);
        } else {
            memcheck_in_internal_query = false;
            active_hook_libs[0] = '\0';
            elog(ERROR, "Unknown scenario: %s", scenario_str);
        }
    }
    PG_CATCH();
    {
        memcheck_in_internal_query = false;
        active_hook_libs[0] = '\0';
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

    active_hook_libs[0] = '\0';

    free_context_tree(before_snapshot_tree);
    free_context_tree(after_snapshot_tree);
    free_context_diff(diffs);

    PG_RETURN_TEXT_P(cstring_to_text("Scenario executed and analyzed. Run 'SELECT * FROM ext_memcheck.end()' to retrieve logged violations."));
}

static void
run_shmem_sentinel_probe(int iterations, const char *workload)
{
    int i;

    if (!ext_track_shmem)
        return; /* shmem tracking disabled for this session via track_shmem:false */

    if (iterations <= 0)
        elog(ERROR, "Iterations must be a positive integer");

    /* Register sentinels for our own shmem segments (allocated with +1 byte). */
    probe_register("pg_ext_memcheck ViolationLog", sizeof(ViolationLog) + 1, sizeof(ViolationLog));
    probe_register("pg_ext_memcheck DsmTrackerState", sizeof(DsmTrackerState) + 1, sizeof(DsmTrackerState));

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

    /*
     * Use plain SPI_connect() — we don't need non-atomic mode because
     * subtransaction management is handled via the internal C API
     * (BeginInternalSubTransaction / RollbackAndReleaseCurrentSubTransaction)
     * rather than through SPI_execute("SAVEPOINT ...").  SPI_execute rejects
     * all transaction-control statements with SPI_ERROR_TRANSACTION (-8)
     * regardless of the connection mode when called from a SQL function.
     */
    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "pg_ext_memcheck: SPI_connect failed");

    for (i = 0; i < iterations; i++) {
        int ret;

        /*
         * Open a subtransaction.  This is equivalent to SAVEPOINT but goes
         * through the internal xact API, which is always available inside a
         * backend regardless of SPI connection mode.
         */
        BeginInternalSubTransaction(NULL);

        ret = SPI_execute(workload, true, 0);
        if (ret != SPI_OK_SELECT)
        {
            /* Roll back the subtransaction before raising the error */
            RollbackAndReleaseCurrentSubTransaction();
            elog(ERROR, "pg_ext_memcheck: SPI_execute failed with code %d", ret);
        }

        /*
         * Intentionally roll back — this is the whole point of tx_abort_loop:
         * simulate a workload whose effects are repeatedly discarded so we can
         * observe whether any memory leaks survive the abort.
         */
        RollbackAndReleaseCurrentSubTransaction();
    }

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
    {
        elog(ERROR, "Failed to attach to DSM segment with handle %u", handle);
        PG_RETURN_NULL();
    }

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

/*
 * shmem_probe_check -- SQL-callable wrapper for probe_check().
 *
 * Returns TRUE if the sentinel byte for seg_name is still 0xDE, FALSE if the
 * byte has been overwritten or the segment is not registered.
 */
PG_FUNCTION_INFO_V1(shmem_probe_check);
Datum
shmem_probe_check(PG_FUNCTION_ARGS)
{
    text *seg_name_text = PG_GETARG_TEXT_PP(0);
    char *seg_name;
    bool  intact;

    if (probe_registry == NULL)
        PG_RETURN_BOOL(false);

    seg_name = text_to_cstring(seg_name_text);
    intact   = probe_check(seg_name);
    PG_RETURN_BOOL(intact);
}

/*
 * shmem_probe_register -- SQL-callable wrapper for probe_register().
 *
 * Registers a sentinel byte just past the declared end of a named shared-memory
 * segment.  allocated_size must be the exact byte count passed to
 * ShmemInitStruct() by the owning extension (i.e. data_size + 1, where the
 * +1 is the sentinel byte that pg_ext_memcheck reserved).
 *
 * Returns a TEXT confirmation message so the caller can verify the C function
 * actually returned a pointer (a VOID mismatch would crash here via detoast).
 */
PG_FUNCTION_INFO_V1(shmem_probe_register);
Datum
shmem_probe_register(PG_FUNCTION_ARGS)
{
    text    *seg_name_text  = PG_GETARG_TEXT_PP(0);
    int64    allocated_size = PG_GETARG_INT64(1);
    char    *seg_name;
    char     msg[256];
    Size     alloc;
    Size     data_end;

    if (allocated_size <= 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pg_ext_memcheck: allocated_size must be greater than 0")));

    seg_name = text_to_cstring(seg_name_text);
    alloc    = (Size) allocated_size;

    /*
     * The sentinel is planted at data_end = alloc_size (the first byte past
     * the declared data), relying on alignment padding for headroom.
     * probe_register() guards data_end < CACHELINEALIGN(alloc_size), so we
     * must reject sizes that are already cache-line-aligned — those have no
     * alignment slack and the guard would fire as an error rather than
     * silently no-op.
     *
     * The caller must either:
     *   a) use a non-CL-aligned size (common for small structs), or
     *   b) allocate their segment with an extra +1 byte and pass
     *      declared_data_size + 1 as allocated_size, matching the convention
     *      used by pg_ext_memcheck's own internal segments.
     */
    if (CACHELINEALIGN(alloc) <= alloc)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pg_ext_memcheck: '%s' allocated_size=%lld is already "
                        "cache-line-aligned; no alignment slack for a sentinel byte. "
                        "Re-allocate the segment with one extra byte "
                        "(RequestAddinShmemSpace(size + 1)) so the sentinel "
                        "has room past your declared data.",
                        seg_name, (long long) allocated_size)));

    /*
     * data_end = alloc_size: sentinel byte at the first byte past the
     * declared data, inside the cache-line alignment padding.
     * probe_register() validates that data_end < CACHELINEALIGN(alloc_size).
     */
    data_end = alloc;
    probe_register(seg_name, alloc, data_end);

    snprintf(msg, sizeof(msg),
             "Registered shmem probe for segment '%s' (allocated_size=%lld bytes)",
             seg_name, (long long) allocated_size);

    PG_RETURN_TEXT_P(cstring_to_text(msg));
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

/*
 * run_context_reset_storm -- repeatedly reset a scratch context then run the workload.
 *
 * For each iteration:
 *   1. Allocate 100×64-byte blocks into a scratch context (simulating extension state).
 *   2. Call MemoryContextReset(), invalidating all pointers into that context.
 *   3. Run the workload via SPI.
 *
 * If the extension under test cached a pointer into the scratch context it may
 * dereference stale memory.  The context-diff snapshot (taken in run_scenario)
 * catches any retained allocation after the storm.  For true crash detection,
 * redirect crash-inducing extensions through use_after_reset (BGWorker).
 *
 * Target bug class: Bug 3 — Use-After-Reset.
 */
static void
run_context_reset_storm(int iterations, const char *workload)
{
    MemoryContext storm_ctx;
    int           i, j;

    if (iterations <= 0)
        elog(ERROR, "Iterations must be a positive integer");

    storm_ctx = AllocSetContextCreate(TopMemoryContext,
                                      "pg_ext_memcheck storm",
                                      ALLOCSET_DEFAULT_SIZES);

    if (SPI_connect() != SPI_OK_CONNECT) {
        MemoryContextDelete(storm_ctx);
        elog(ERROR, "pg_ext_memcheck: SPI_connect failed");
    }

    for (i = 0; i < iterations; i++)
    {
        MemoryContext old;
        int           ret;

        /* Simulate extension allocating state into the context */
        old = MemoryContextSwitchTo(storm_ctx);
        for (j = 0; j < 100; j++)
            palloc(64);
        MemoryContextSwitchTo(old);

        /* Invalidate all pointers — extension should not hold references here */
        MemoryContextReset(storm_ctx);

        /* Run the workload; extension may access state it allocated above */
        ret = SPI_execute(workload, true, 0);
        if (ret != SPI_OK_SELECT)
        {
            SPI_finish();
            MemoryContextDelete(storm_ctx);
            elog(ERROR, "pg_ext_memcheck: SPI_execute failed with code %d", ret);
        }
    }

    SPI_finish();
    MemoryContextDelete(storm_ctx);
}

/*
 * run_cursor_leak -- open N SPI cursors (portals) without closing them, run workload.
 *
 * Extensions that open cursors internally and fail to close them cause portal
 * contexts to accumulate.  This scenario simulates that pattern: N cursors are
 * opened and left open while the workload executes.  The snapshot diff (taken in
 * run_scenario) reports PortalContext child growth as context_leak violations.
 * Cursors are drained and closed via SPI_finish() at the end so the backend
 * is left in a clean state.
 *
 * iterations controls both the number of open cursors (capped at 32) and the
 * number of workload executions while they are open.
 *
 * Target bug class: Bug 1 — MemoryContext Leak / Bug 5 — DSM Segment Leak.
 */
static void
run_cursor_leak(int iterations, const char *workload)
{
    int    n_portals;
    int    i;

    if (iterations <= 0)
        elog(ERROR, "Iterations must be a positive integer");

    n_portals = (iterations < 32) ? iterations : 32;

    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "pg_ext_memcheck: SPI_connect failed");

    /* Open n_portals cursors without closing them */
    for (i = 0; i < n_portals; i++)
    {
        char   cursor_name[64];
        Portal p;

        snprintf(cursor_name, sizeof(cursor_name), "pg_ext_memcheck_leak_%d", i);
        p = SPI_cursor_open_with_args(cursor_name, workload,
                                      0, NULL, NULL, NULL, true, 0);
        if (p == NULL)
        {
            elog(WARNING, "pg_ext_memcheck: cursor %d failed to open, stopping at %d cursors",
                 i, i);
            break;
        }
    }

    elog(INFO, "pg_ext_memcheck cursor_leak: %d cursors open", n_portals);

    /* Run workload while cursors remain open, simulating concurrent portal usage */
    for (i = 0; i < iterations; i++)
    {
        int ret = SPI_execute(workload, true, 0);
        if (ret != SPI_OK_SELECT)
        {
            SPI_finish();
            elog(ERROR, "pg_ext_memcheck: SPI_execute failed with code %d", ret);
        }
    }

    /* SPI_finish closes all open portals/cursors owned by this connection */
    SPI_finish();
}

/*
 * run_cold_warm_cold -- run workload, sleep, run again; detect CacheMemoryContext misuse.
 *
 * Phase layout (each phase = iterations/3, minimum 1):
 *   Cold  : first batch — extension caches into CacheMemoryContext
 *   Sleep : 1-second pg_sleep — simulates idle time between sessions
 *   Warm  : second batch — exercise extension with warm cache
 *
 * The snapshot diff across the entire scenario reveals whether CacheMemoryContext
 * (or a child of it) grows monotonically across the cold→warm boundary, which
 * would indicate the extension is accumulating cache entries without eviction.
 *
 * Target bug class: Bug 6 — Monotonic Context Growth.
 */
static void
run_cold_warm_cold(int iterations, const char *workload)
{
    int phase_iters;
    int i;

    if (iterations <= 0)
        elog(ERROR, "Iterations must be a positive integer");

    phase_iters = iterations / 3;
    if (phase_iters < 1)
        phase_iters = 1;

    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "pg_ext_memcheck: SPI_connect failed");

    /* Cold phase */
    elog(INFO, "pg_ext_memcheck cold_warm_cold: cold phase (%d iters)", phase_iters);
    for (i = 0; i < phase_iters; i++)
    {
        int ret = SPI_execute(workload, true, 0);
        if (ret != SPI_OK_SELECT)
        {
            SPI_finish();
            elog(ERROR, "pg_ext_memcheck: SPI_execute failed (cold) with code %d", ret);
        }
    }

    /* Idle phase — let backend sit idle to age cached contexts */
    elog(INFO, "pg_ext_memcheck cold_warm_cold: idle phase (1 s)");
    SPI_execute("SELECT pg_sleep(1)", false, 0);

    /* Warm phase */
    elog(INFO, "pg_ext_memcheck cold_warm_cold: warm phase (%d iters)", phase_iters);
    for (i = 0; i < phase_iters; i++)
    {
        int ret = SPI_execute(workload, true, 0);
        if (ret != SPI_OK_SELECT)
        {
            SPI_finish();
            elog(ERROR, "pg_ext_memcheck: SPI_execute failed (warm) with code %d", ret);
        }
    }

    SPI_finish();
}

/*
 * run_concurrent_backends -- launch N BGWorkers sequentially, each running the workload.
 *
 * True concurrent access would require synchronisation infrastructure beyond the
 * single WorkerSlot.  This implementation exercises the same code paths across N
 * separate backend processes (sequentially), which:
 *   - validates that each backend can attach to and read/write extension shmem
 *     without corrupting the sentinel byte
 *   - catches crashes (SIGSEGV, ERROR) that only manifest in a fresh backend
 *     (e.g., missing shmem re-attach on reconnect)
 *
 * violations written:
 *   concurrent_backends / ERROR  — if any worker exits non-zero
 *
 * Target bug class: Bug 4 — Shmem Segment Overrun.
 */
static void
run_concurrent_backends(int iterations, const char *workload)
{
    int i;
    int n_workers;

    if (iterations <= 0)
        elog(ERROR, "Iterations must be a positive integer");

    n_workers = (iterations < 16) ? iterations : 16;

    elog(INFO, "pg_ext_memcheck concurrent_backends: launching %d workers", n_workers);

    for (i = 0; i < n_workers; i++)
    {
        elog(INFO, "pg_ext_memcheck concurrent_backends: worker %d/%d", i + 1, n_workers);
        launch_workload_worker(workload);
    }
}

/*
 * run_use_after_reset -- helper to launch use_after_reset via BGWorker
 */
static void
run_use_after_reset(int iterations, const char *workload)
{
    launch_crash_isolation_worker("use_after_reset");
}

/*
 * run_oom_simulation -- helper to launch oom_simulation via BGWorker
 */
static void
run_oom_simulation(int iterations, const char *workload)
{
    launch_crash_isolation_worker("oom_simulation");
}
