/*-------------------------------------------------------------------------
 * pg_ext_memcheck
 *
 * Copyright (c) 2026, Samba Siva Reddy
 *
 * This software is released under the MIT License.
 * See LICENSE for details.
 *-------------------------------------------------------------------------
*/
#ifndef WORKER_HARNESS_H
#define WORKER_HARNESS_H

#include "postgres.h"
#include "storage/lwlock.h"

#define WORKER_SCENARIO_LEN  64
#define WORKER_DATABASE_LEN  NAMEDATALEN

typedef enum WorkerStatus {
    WORKER_STATUS_IDLE    = 0,
    WORKER_STATUS_RUNNING = 1,
    WORKER_STATUS_DONE    = 2,
    WORKER_STATUS_CRASHED = 3
} WorkerStatus;

#define WORKER_WORKLOAD_LEN  256

typedef struct WorkerSlot {
    LWLock       lock;
    WorkerStatus status;
    char         scenario[WORKER_SCENARIO_LEN];   /* e.g. "use_after_reset" */
    char         database[WORKER_DATABASE_LEN];   /* DB for the worker to connect to */
    char         workload[WORKER_WORKLOAD_LEN];   /* SQL for run_workload scenario */
    int          requestor_pid;                   /* PID of the launching backend */
    int          exit_code;                       /* non-zero → crash */
    int          n_workers;                       /* number of concurrent workers (concurrent_backends) */
} WorkerSlot;

extern WorkerSlot *worker_slot;
extern int worker_harness_tranche_id;

/* Called from _PG_init to register the worker library */
extern void worker_harness_init(void);

/* Called from sql_api.c scenario dispatch */
extern void launch_crash_isolation_worker(const char *scenario);
extern void launch_workload_worker(const char *workload);

/* BGWorker entry point — must be extern and visible for RegisterDynamicBackgroundWorker */
extern void bgworker_harness_main(Datum main_arg);

#endif /* WORKER_HARNESS_H */
