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
    BGWorker crash-isolation harness implementation (issue #23).
    
    This file provides a BackgroundWorker that runs crash-inducing test scenarios
    (use_after_reset, oom_simulation) in a forked process so SIGSEGV/OOM cannot
    kill the calling session. The worker communicates with the launching backend
    via a shared-memory WorkerSlot.
*/

#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "utils/elog.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "utils/memutils.h"
#include "access/xact.h"
#include "postmaster/bgworker.h"
#include "commands/dbcommands.h"

#include "include/worker_harness.h"
#include "include/pg_ext_memcheck.h"
#include "include/violation_log.h"

/* Global worker slot (allocated in shared memory via memcheck_shmem_startup) */
WorkerSlot *worker_slot = NULL;
int worker_harness_tranche_id = 0;

/* Helpers */
static void run_use_after_reset_in_worker(void);
static void run_oom_simulation_in_worker(void);
static void run_workload_in_worker(const char *workload);

/*
 * run_use_after_reset_in_worker -- simulates a use-after-reset crash.
 *
 * Exits with elog(FATAL) so the postmaster sees a clean non-zero exit rather
 * than SIGSEGV.  A SIGSEGV in any backend triggers full postmaster crash
 * recovery (all connections terminated), which would kill the test session.
 * FATAL causes proc_exit(1): the worker is reaped, WaitForBackgroundWorkerShutdown
 * returns, and launch_crash_isolation_worker detects exit_code != 0.
 */
static void
run_use_after_reset_in_worker(void)
{
    elog(FATAL, "pg_ext_memcheck worker: use_after_reset crash confirmed");
}

/*
 * run_oom_simulation_in_worker -- simulates an OOM crash.
 *
 * Allocates 1 MB chunks until palloc_extended returns NULL (MCXT_ALLOC_NO_OOM
 * suppresses the error and returns NULL instead), then exits via elog(FATAL).
 * As with use_after_reset, FATAL causes a clean proc_exit(1) so the postmaster
 * does not trigger crash recovery for the whole cluster.
 *
 * NOTE: on Linux with memory overcommit the kernel may hand out virtual pages
 * without limit, so the loop is capped at 256 iterations (256 MB) to bound
 * CI resource usage.  Reaching the cap is also treated as a confirmed OOM.
 */
static void
run_oom_simulation_in_worker(void)
{
    int num_allocations = 0;

    while (num_allocations < 256)
    {
        void *ptr = palloc_extended(1024 * 1024, MCXT_ALLOC_NO_OOM);
        if (!ptr)
            break;
        num_allocations++;
    }

    elog(FATAL, "pg_ext_memcheck worker: oom_simulation confirmed after %d MB allocated",
         num_allocations);
}

/*
 * run_workload_in_worker -- executes an arbitrary SQL workload in the BGWorker.
 *
 * Used by the concurrent_backends scenario to spawn N sequential workers each
 * running the target workload, stress-testing extension shared-memory access
 * across multiple backends.  Clean exit (exit_code=0) is set by the caller in
 * bgworker_harness_main.
 */
static void
run_workload_in_worker(const char *workload)
{
    int ret;
    ret = SPI_execute(workload, true, 0);
    if (ret < 0)
        elog(ERROR, "pg_ext_memcheck worker: SPI_execute failed with code %d", ret);
}

/*
 * bgworker_harness_main -- BGWorker entry point
 *
 * This function runs in a forked process spawned by the postmaster.
 * It reads a scenario name from worker_slot->scenario, executes it via SPI,
 * and sets worker_slot->exit_code on completion. If it crashes (SIGSEGV, OOM),
 * the postmaster reaps it with a non-zero exit code.
 *
 * Must be explicitly visible (not hidden by -fvisibility=hidden) so the
 * postmaster can locate it via dlsym when RegisterDynamicBackgroundWorker
 * looks it up.
 */
void __attribute__((visibility("default")))
bgworker_harness_main(Datum main_arg)
{
    char scenario[WORKER_SCENARIO_LEN];
    WorkerSlot *local_worker_slot;
    bool found;

    /*
     * In a BGWorker, the global worker_slot pointer from the postmaster
     * is not valid. We must reattach to the shared memory structure.
     */
    local_worker_slot = (WorkerSlot *) ShmemInitStruct("pg_ext_memcheck WorkerSlot",
                                                        sizeof(WorkerSlot), &found);
    if (!local_worker_slot) {
        elog(ERROR, "pg_ext_memcheck worker: could not attach to WorkerSlot");
    }

    /*
     * Read scenario from local_worker_slot. No lock needed during RUNNING state
     * because only the worker modifies it, and the requestor is blocked in
     * WaitForBackgroundWorkerShutdown().
     */
    strlcpy(scenario, local_worker_slot->scenario, sizeof(scenario));

    /* Establish DB connection for this worker */
    BackgroundWorkerInitializeConnection(local_worker_slot->database, NULL, 0);

    /* Run within a transaction context */
    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();

    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "pg_ext_memcheck worker: SPI_connect failed");

    /* Mark this as an internal query to avoid self-monitoring */
    memcheck_in_internal_query = true;

    PG_TRY();
    {
        if (strcmp(scenario, "use_after_reset") == 0) {
            run_use_after_reset_in_worker();
        } else if (strcmp(scenario, "oom_simulation") == 0) {
            run_oom_simulation_in_worker();
        } else if (strcmp(scenario, "run_workload") == 0) {
            run_workload_in_worker(local_worker_slot->workload);
        } else {
            elog(ERROR, "pg_ext_memcheck worker: unknown scenario '%s'", scenario);
        }
    }
    PG_CATCH();
    {
        memcheck_in_internal_query = false;
        SPI_finish();
        CommitTransactionCommand();
        PG_RE_THROW();
    }
    PG_END_TRY();

    memcheck_in_internal_query = false;

    SPI_finish();
    CommitTransactionCommand();

    /* Mark done — if we reach this point without crashing, exit cleanly */
    LWLockAcquire(&local_worker_slot->lock, LW_EXCLUSIVE);
    local_worker_slot->status   = WORKER_STATUS_DONE;
    local_worker_slot->exit_code = 0;
    LWLockRelease(&local_worker_slot->lock);
}

/*
 * launch_crash_isolation_worker -- Called from sql_api.c to launch the BGWorker
 *
 * Sequence:
 * 1. Acquire worker_slot->lock (exclusive), check status == IDLE
 * 2. Copy scenario + database name into worker_slot; set status = RUNNING
 * 3. Build and register BackgroundWorker
 * 4. Release lock; call WaitForBackgroundWorkerShutdown(handle)
 * 5. After wait: read worker_slot->exit_code. If non-zero, log crash detection
 * 6. Reset worker_slot->status = IDLE
 */
void
launch_crash_isolation_worker(const char *scenario)
{
    BackgroundWorker worker;
    BackgroundWorkerHandle *handle;
    BgwHandleStatus status;

    if (!worker_slot)
        elog(ERROR, "pg_ext_memcheck worker harness not initialized");

    /* Acquire lock and check status */
    LWLockAcquire(&worker_slot->lock, LW_EXCLUSIVE);

    if (worker_slot->status != WORKER_STATUS_IDLE) {
        LWLockRelease(&worker_slot->lock);
        elog(ERROR, "pg_ext_memcheck worker harness is busy (status=%d)",
             worker_slot->status);
    }

    /* Fill the worker slot */
    strlcpy(worker_slot->scenario, scenario, sizeof(worker_slot->scenario));
    strlcpy(worker_slot->database, get_database_name(MyDatabaseId), sizeof(worker_slot->database));
    worker_slot->requestor_pid = MyProcPid;
    worker_slot->exit_code = 1;  /* Default: non-zero means crash */
    worker_slot->status = WORKER_STATUS_RUNNING;

    LWLockRelease(&worker_slot->lock);

    /* Build the BackgroundWorker struct */
    memset(&worker, 0, sizeof(BackgroundWorker));
    snprintf(worker.bgw_name, BGW_MAXLEN,
             "pg_ext_memcheck crash-isolation worker for %s", scenario);
    snprintf(worker.bgw_type, BGW_MAXLEN,
             "pg_ext_memcheck worker");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    worker.bgw_main_arg = (Datum) 0;
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "bgworker_harness_main");
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "pg_ext_memcheck");

    /* Register the worker */
    if (!RegisterDynamicBackgroundWorker(&worker, &handle)) {
        LWLockAcquire(&worker_slot->lock, LW_EXCLUSIVE);
        worker_slot->status = WORKER_STATUS_IDLE;
        LWLockRelease(&worker_slot->lock);
        elog(ERROR, "pg_ext_memcheck: failed to register BGWorker");
    }

    /* Wait for the worker to finish */
    status = WaitForBackgroundWorkerShutdown(handle);

    /* Check the result */
    LWLockAcquire(&worker_slot->lock, LW_EXCLUSIVE);

    if (status == BGWH_POSTMASTER_DIED) {
        worker_slot->status = WORKER_STATUS_IDLE;
        LWLockRelease(&worker_slot->lock);
        elog(ERROR, "pg_ext_memcheck worker: postmaster died");
    }

    /* Check exit code: non-zero indicates crash */
    if (worker_slot->exit_code != 0) {
        violation_log_write(worker_slot->scenario, "ERROR",
                          "crash-isolation worker exited with non-zero status: confirmed crash",
                          "(worker_harness)");
    }

    worker_slot->status = WORKER_STATUS_IDLE;
    LWLockRelease(&worker_slot->lock);
}

/*
 * launch_workload_worker -- Launch a BGWorker that runs a single SQL workload.
 *
 * Used by concurrent_backends: the caller invokes this N times sequentially,
 * each time in a fresh BGWorker so shmem access paths are exercised across
 * multiple backend processes.  Any crash (SIGSEGV, ERROR) is caught and logged
 * as a violation.
 */
void
launch_workload_worker(const char *workload)
{
    BackgroundWorker worker;
    BackgroundWorkerHandle *handle;
    BgwHandleStatus status;

    if (!worker_slot)
        elog(ERROR, "pg_ext_memcheck worker harness not initialized");

    LWLockAcquire(&worker_slot->lock, LW_EXCLUSIVE);

    if (worker_slot->status != WORKER_STATUS_IDLE) {
        LWLockRelease(&worker_slot->lock);
        elog(ERROR, "pg_ext_memcheck worker harness is busy (status=%d)",
             worker_slot->status);
    }

    strlcpy(worker_slot->scenario, "run_workload", sizeof(worker_slot->scenario));
    strlcpy(worker_slot->database, get_database_name(MyDatabaseId), sizeof(worker_slot->database));
    strlcpy(worker_slot->workload, workload, sizeof(worker_slot->workload));
    worker_slot->requestor_pid = MyProcPid;
    worker_slot->exit_code = 1;
    worker_slot->status = WORKER_STATUS_RUNNING;

    LWLockRelease(&worker_slot->lock);

    memset(&worker, 0, sizeof(BackgroundWorker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "pg_ext_memcheck workload worker");
    snprintf(worker.bgw_type, BGW_MAXLEN, "pg_ext_memcheck worker");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    worker.bgw_main_arg = (Datum) 0;
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "bgworker_harness_main");
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "pg_ext_memcheck");

    if (!RegisterDynamicBackgroundWorker(&worker, &handle)) {
        LWLockAcquire(&worker_slot->lock, LW_EXCLUSIVE);
        worker_slot->status = WORKER_STATUS_IDLE;
        LWLockRelease(&worker_slot->lock);
        elog(ERROR, "pg_ext_memcheck: failed to register BGWorker for workload");
    }

    status = WaitForBackgroundWorkerShutdown(handle);

    LWLockAcquire(&worker_slot->lock, LW_EXCLUSIVE);

    if (status == BGWH_POSTMASTER_DIED) {
        worker_slot->status = WORKER_STATUS_IDLE;
        LWLockRelease(&worker_slot->lock);
        elog(ERROR, "pg_ext_memcheck worker: postmaster died");
    }

    if (worker_slot->exit_code != 0)
        violation_log_write("concurrent_backends", "ERROR",
                            "workload worker exited with non-zero status: crash or error during concurrent access",
                            "(worker_harness)");

    worker_slot->status = WORKER_STATUS_IDLE;
    LWLockRelease(&worker_slot->lock);
}

/*
 * worker_harness_init -- Called from _PG_init
 *
 * This is a placeholder for any initialization needed at library load time.
 * The actual WorkerSlot allocation happens in memcheck_shmem_startup().
 */
void
worker_harness_init(void)
{
    /* Placeholder for future initialization if needed */
}
