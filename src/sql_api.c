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

// Local Includes
#include "include/pg_ext_memcheck.h"
#include "include/gucs.h"
#include "include/violation_log.h"
#include "include/context_walker.h"
#include "include/memcheck_hooks.h"
#include "include/dsm_tracker.h"

// Static function declarations
static void run_growth_benchmark(int iterations, const char *workload);
static void run_tx_abort_loop(int iterations, const char *workload);

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

        entries = violation_log_read_all();
        if (entries != NULL)
        {
            for (i = 0; i < MEMCHECK_MAX_VIOLATIONS; i++)
            {
                ViolationEntry *e = &entries[i];

                /* Skip slots that have never been written (check_type is empty). */
                if (e->check_type[0] == '\0')
                    continue;

                values[0] = CStringGetTextDatum(e->check_type);
                values[1] = CStringGetTextDatum(e->severity);
                values[2] = CStringGetTextDatum(e->detail);
                values[3] = TimestampTzGetDatum(e->ts);
                values[4] = CStringGetTextDatum(e->source_lib);

                tuplestore_putvalues(tupstore, tupdesc, values, nulls);
            }
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
    text *scenario_text = PG_GETARG_TEXT_PP(0);
    char *scenario_str = text_to_cstring(scenario_text);
    // Default iterations, can be overridden by argument
    int iterations = PG_NARGS() > 1 ? PG_GETARG_INT32(1) : 100; 
    text *workload_text = PG_NARGS() > 2 ? PG_GETARG_TEXT_PP(2) : NULL;
    char *workload_str = workload_text ? text_to_cstring(workload_text) : "SELECT 1";

    // Placeholder for actual scenario execution logic
    elog(INFO, "Running memory check scenario: %s", scenario_str); 
    elog(INFO, "Iterations: %d", iterations);
    elog(INFO, "Workload: %s", workload_str);

    CtxTree *before_snapshot_tree = snapshot_context_tree(TopMemoryContext); 
    memcheck_in_internal_query = true; // Set flag to indicate we're running an internal scenario query

    if (strcmp(scenario_str, "growth_benchmark") == 0) {
        run_growth_benchmark(iterations, workload_str);
    } else if (strcmp(scenario_str, "tx_abort_loop") == 0) {
        run_tx_abort_loop(iterations, workload_str);
    } else {
        elog(ERROR, "Unknown scenario: %s", scenario_str);
    }

    memcheck_in_internal_query = false; // Reset flag after scenario execution
    CtxTree *after_snapshot_tree = snapshot_context_tree(TopMemoryContext);

    // Analyze differences between before and after snapshots, log any detected issues.
    int diff_count = 0;
    CtxDiff *diffs = diff_context_trees(before_snapshot_tree, after_snapshot_tree, &diff_count);
    for (int i = 0; i < diff_count; i++) {
        analyze_and_log_diff(&diffs[i]);
    }
    // Check for wrong context allocations as well
    check_wrong_context_alloc(before_snapshot_tree, after_snapshot_tree); 

    PG_RETURN_TEXT_P(cstring_to_text("Scenario executed and analyzed. Run 'SELECT * FROM ext_memcheck.end()' to retrieve logged violations."));
}

static void 
run_growth_benchmark(int iterations, const char *workload) {
    if (iterations <= 0) {
        elog(ERROR, "Iterations must be a positive integer");
        return;
    }

    if (SPI_connect() != SPI_OK_CONNECT) {
        elog(ERROR, "pg_ext_memcheck: SPI_connect failed");
        return;
    }

    for (int i = 0; i < iterations; i++) {
        int ret = SPI_execute(workload, true, 0);
        if (ret != SPI_OK_SELECT) {
            elog(ERROR, "pg_ext_memcheck: SPI_execute failed with code %d", ret);
            SPI_finish();
            return;
        }
    }

    SPI_finish();
}

static void 
run_tx_abort_loop(int iterations, const char *workload) {
    if (iterations <= 0) {
        elog(ERROR, "Iterations must be a positive integer");
        return;
    }

    if (SPI_connect() != SPI_OK_CONNECT) {
        elog(ERROR, "pg_ext_memcheck: SPI_connect failed");
        return;
    }

    for (int i = 0; i < iterations; i++) {
        // Start a transaction, then immediately abort it to test memory cleanup on transaction abort.
        int ret = SPI_execute("SAVEPOINT _memcheck_sp", false, 0);
        if (ret != SPI_OK_UTILITY) {
            elog(ERROR, "pg_ext_memcheck: SPI_execute failed with code %d", ret);
            SPI_finish();
            return;
        }

        ret = SPI_execute(workload, true, 0);
        if (ret != SPI_OK_SELECT) {
            elog(ERROR, "pg_ext_memcheck: SPI_execute failed with code %d", ret);
            SPI_finish();
            return;
        }

        ret = SPI_execute("ROLLBACK TO SAVEPOINT _memcheck_sp", false, 0);
        if (ret != SPI_OK_UTILITY) {
            elog(ERROR, "pg_ext_memcheck: SPI_execute failed with code %d", ret);
            SPI_finish();
            return;
        }
    }

    SPI_execute("RELEASE SAVEPOINT _memcheck_sp", false, 0); // Clean up savepoint after loop

    SPI_finish();
}

PG_FUNCTION_INFO_V1(dsm_tracker_list_segments);
Datum
dsm_tracker_list_segments(PG_FUNCTION_ARGS)
{
    if (dsm_tracker_state == NULL)
        PG_RETURN_NULL(); /* tracker not initialized, should not happen but be defensive */

    ReturnSetInfo   *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc        tupdesc;
    Tuplestorestate *tupstore;
    int              i;

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

            values[0] = Int64GetDatum((int64) r->handle);
            values[1] = Int32GetDatum(r->backend_pid);
            values[2] = TimestampTzGetDatum(r->attached_at);
            values[3] = Int64GetDatum((int64) r->size_bytes);
            values[4] = BoolGetDatum(r->detached);

            tuplestore_putvalues(tupstore, tupdesc, values, nulls);
        }

        MemoryContextSwitchTo(oldcontext);
    }

    PG_RETURN_NULL();
}