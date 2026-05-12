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
#include "utils/tuplestore.h"

// Local Includes
#include "include/pg_ext_memcheck.h"
#include "include/gucs.h"
#include "include/violation_log.h"
#include "include/context_walker.h"

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
 * Defintion: CREATE OR REPLACE FUNCTION ext_memcheck.end()
    RETURNS TABLE(check_type TEXT, severity TEXT, detail TEXT, ts TIMESTAMPTZ)
    AS 'pg_ext_memcheck', 'memcheck_end'
    LANGUAGE C STRICT;
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

    /* Verify caller can accept a set result */
    if (!rsinfo || !IsA(rsinfo, ReturnSetInfo) ||
        !(rsinfo->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that cannot accept a set")));

    /* Build and bless a tuple descriptor for the result set */
    tupdesc = CreateTemplateTupleDesc(4);
    TupleDescInitEntry(tupdesc, (AttrNumber) 1, "check_type", TEXTOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber) 2, "severity", TEXTOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber) 3, "detail", TEXTOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber) 4, "ts", TIMESTAMPTZOID, -1, 0);
    BlessTupleDesc(tupdesc);

    tupstore = tuplestore_begin_heap(true, false, work_mem);

    entries = violation_log_read_all();
    if (entries != NULL)
    {
        Datum   values[4];
        bool    nulls[4] = {false, false, false, false};

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

            tuplestore_putvalues(tupstore, tupdesc, values, nulls);
        }
    }

    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult = tupstore;
    rsinfo->setDesc = tupdesc;

    PG_RETURN_NULL();
}
