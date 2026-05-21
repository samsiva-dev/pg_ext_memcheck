# pg_ext_memcheck
[![CI](https://github.com/samsiva-dev/pg_ext_memcheck/actions/workflows/ci.yml/badge.svg)](https://github.com/samsiva-dev/pg_ext_memcheck/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/samsiva-dev/pg_ext_memcheck)](LICENSE)

> **Development preview — not production-safe.** APIs may change between releases.

Instrument and stress-test the memory behavior of PostgreSQL extensions — from inside the backend process.

Tools like Valgrind and AddressSanitizer are blind to PostgreSQL's internal memory model. They can't tell you that a `palloc()` went into the wrong `MemoryContext`, that a context leaked across a query boundary, or that shared memory sentinels were overwritten by a buggy extension.

**pg_ext_memcheck runs inside the backend process**, giving it full visibility into the `MemoryContext` tree and PostgreSQL's internal allocators.

| Bug class | Valgrind | ASan | pg_ext_memcheck |
|---|---|---|---|
| MemoryContext leak | ✗ | ✗ | ✓ |
| Wrong-context palloc | ✗ | ✗ | ✓ |
| Shmem boundary overrun | ± | ± | ✓ |
| DSM segment leak | ✗ | ✗ | ✓ |
| Use-after-reset bug | ✗ | ✗ | ✓ |
| Context growth / bloat | ✗ | ✗ | ✓ |
| Heap use-after-free | ✓ | ✓ | ✗ |

---

## What it detects

- **Context leaks** — Snapshots the `MemoryContext` tree before and after a query, then diffs it to surface contexts that were created but never freed.
- **Wrong-context allocations** — Flags `palloc()` calls that land in long-lived contexts like `TopMemoryContext` or `CacheMemoryContext` when they should be query-local.
- **Context bloat** — Measures monotonic growth across repeated invocations to detect slow, cumulative leaks.
- **Shmem overruns** — Plants sentinel bytes around shared memory allocations and verifies their integrity after extension code runs.
- **DSM lifecycle** — Tracks DSM segment attach and detach calls to detect segments that are attached but never released.
- **Use-after-reset** *(Phase 2)* — Forces a context reset then re-invokes the extension function to expose dangling pointer dereferences.

---

## Prerequisites

- PostgreSQL 15 or later (server headers required)
- `pg_config` in your `PATH`
- A C compiler (`gcc` or `clang`)

---

## Installation

```bash
git clone https://github.com/samsiva-dev/pg_ext_memcheck.git
cd pg_ext_memcheck
make
sudo make install
```

Add the extension to `postgresql.conf` and restart PostgreSQL:

```ini
shared_preload_libraries = 'pg_ext_memcheck'
```

Then create the extension in your database:

```sql
CREATE EXTENSION pg_ext_memcheck;
```

---

## Quickstart

```sql
-- Start a check window in executor mode
SELECT ext_memcheck.begin('executor');

-- Call the function you want to inspect
SELECT your_extension.some_function('input');

-- End the window; returns violations detected during this window
SELECT * FROM ext_memcheck.end();

-- Flush ring-buffer entries to the persistent log table
SELECT ext_memcheck.flush_violations();

-- Query the violation log for details
SELECT * FROM ext_memcheck.violation_log ORDER BY ts DESC;
```

A violation row looks like this:

| Column | Example |
|---|---|
| `id` | `1` |
| `ts` | `2026-05-10 14:32:01 UTC` |
| `backend_pid` | `12345` |
| `check_type` | `context_leak`, `wrong_ctx_alloc`, `shmem_overrun`, `dsm_leak` |
| `severity` | `ERROR`, `WARNING`, `INFO` |
| `detail` | `"Context present in post-query snapshot but not pre-query"` |
| `source_lib` | `your_extension.dylib` |

### Severity thresholds

| Level | Condition |
|---|---|
| `ERROR` | Net context growth ≥ 1 MiB |
| `WARNING` | Net context growth ≥ 64 KiB and < 1 MiB |
| `INFO` | Net context growth ≥ `min_leak_bytes` (default 8 KiB) |

---

## Stress scenarios

`growth_benchmark`, `tx_abort_loop`, and `shmem_sentinel_probe` are available now. Additional scenarios (`context_reset_storm`, `concurrent_backends`, `dsm_lifecycle_check`, `wrong_context_probe`) are planned for Phase 2.

```sql
-- Measures context growth over repeated calls — catches slow cumulative leaks
SELECT ext_memcheck.run_scenario(scenario_name := 'growth_benchmark', iterations := 100, workload := 'SELECT your_extension.some_function(''input'');');

-- Tests memory cleanup on transaction abort
SELECT ext_memcheck.run_scenario(scenario_name := 'tx_abort_loop', iterations := 50, workload := 'SELECT 1');

-- Plants sentinel bytes past shmem boundaries and verifies integrity after the workload
SELECT ext_memcheck.run_scenario('shmem_sentinel_probe', 10, 'SELECT 1');

SELECT ext_memcheck.flush_violations();
```

| Scenario | What it catches |
|---|---|
| `growth_benchmark` | Slow cumulative leaks; monotonic context bloat across repeated calls |
| `tx_abort_loop` | Context leaks that only manifest on transaction abort; resources not cleaned up on rollback |
| `shmem_sentinel_probe` | Off-by-one writes past a segment's declared shmem boundary |

---

## GUC parameters

Set in `postgresql.conf` or with `SET` at session scope (no restart required).

| Parameter | Type | Default | Description |
|---|---|---|---|
| `pg_ext_memcheck.memcheck_mode` | `enum` | `all` | `all` / `executor` / `none` — controls which execution phases are hooked. |
| `pg_ext_memcheck.min_leak_bytes` | `int` | `8192` | Context growth smaller than this (bytes) is silently ignored. |

```sql
-- Check both planner and executor phases
SET pg_ext_memcheck.memcheck_mode = 'all';

-- Focus on executor phase only (reduces noise for targeted testing)
SET pg_ext_memcheck.memcheck_mode = 'executor';

-- Disable all instrumentation (zero overhead)
SET pg_ext_memcheck.memcheck_mode = 'none';
```

---

## SQL API reference

| Function | Returns | Description |
|---|---|---|
| `ext_memcheck.begin(target_mode TEXT)` | `text` | Opens a test window and sets `memcheck_mode`. |
| `ext_memcheck.end()` | `TABLE(check_type, severity, detail, ts, source_lib)` | Closes the window and returns violations detected. Does not flush to `violation_log`. |
| `ext_memcheck.flush_violations()` | `int` | Drains the ring buffer into `violation_log`; returns count flushed. |
| `ext_memcheck.run_scenario(scenario_name TEXT, iterations INT, workload TEXT)` | `text` | Runs a named stress scenario with a custom workload query. |
| `ext_memcheck.clear_violations()` | `void` | Clears all rows from the `violation_log` table (does not affect ring buffer). |
| `ext_memcheck.track_dsm_handle(handle BIGINT)` | `text` | Registers a DSM handle for lifecycle tracking. |
| `ext_memcheck.dsm_tracking()` | `TABLE(segid, backend_pid, attach_at, size_bytes, detached)` | Returns all currently tracked DSM segments. |
| `ext_memcheck.clear_dsm_tracking()` | `void` | Resets the DSM tracking table between test runs. |
| `ext_memcheck.clear_shmem_registry()` | `void` | Resets the shmem sentinel probe registry between test runs. |

The ring buffer is capped at 256 entries (oldest-first eviction when full). Call `flush_violations()` regularly to avoid data loss.

---

## Testing a leaky extension

pg_ext_memcheck ships with a companion buggy extension ([buggy-pg-ext](https://github.com/samsiva-dev/buggy-pg-ext)) that intentionally leaks memory to demonstrate the tool.

```sql
CREATE EXTENSION buggy_pg_ext;
CREATE EXTENSION pg_ext_memcheck;
SET pg_ext_memcheck.memcheck_mode = 'all';

-- Any query will trigger the buggy extension's hooks
SELECT count(*) FROM pg_class;

SELECT * FROM ext_memcheck.flush_violations();
SELECT * FROM ext_memcheck.violation_log;
```

Run the growth benchmark to see severity escalate over 1000 iterations:

```sql
SELECT ext_memcheck.begin('all');
SELECT ext_memcheck.run_scenario(scenario_name := 'growth_benchmark', iterations := 1000, workload := 'SELECT count(*) FROM pg_class;');
SELECT * FROM ext_memcheck.end();
```

After 1000 iterations the `TopMemoryContext` leak (~8 MB) escalates to `ERROR`; the wrong-context allocation fires as `WARNING`; the planner leak ctx stays `INFO` (< 64 KiB). See the [full walkthrough](https://pg-ext-memcheck.vercel.app/testing-buggy-extensions/) on the docs site.

---

## Architecture

pg_ext_memcheck is composed of eight C modules loaded via `shared_preload_libraries`. No PostgreSQL source patching is required.

```
┌─────────────────────────────────────────────────────┐
│                   Backend Process                    │
│                                                      │
│  SQL Layer  ──►  memcheck_hooks.c  (executor hooks) │
│                       │                              │
│          ┌────────────┼────────────┐                 │
│          ▼            ▼            ▼                 │
│  context_walker   shmem_probe   dsm_tracker          │
│      (Phase 1)    (Phase 1)     (Phase 1)            │
│          │                                           │
│          ▼                                           │
│   violation_log.c  (shared ring buffer)              │
│          │                                           │
│          ▼                                           │
│   SQL: flush_violations() ──► violations table       │
│                                                      │
│  worker_harness.c  (background worker, Phase 2)      │
│  gucs.c            (GUC parameters)                  │
└─────────────────────────────────────────────────────┘
```

### Module summary

| Module | Role |
|---|---|
| `memcheck_hooks.c` | Registers `ExecutorStart`, `ExecutorEnd`, and `planner_hook`; brackets every query with pre/post snapshots |
| `context_walker.c` | Walks the `MemoryContext` tree; produces snapshots and diffs them to find leaks and bloat |
| `violation_log.c` | Manages the 256-entry shared ring buffer (LWLock-protected); exposed via `flush_violations()` |
| `shmem_probe.c` | Plants `0xDE` sentinel bytes past shmem boundaries; detects overruns post-workload |
| `dsm_tracker.c` | Records DSM attach/detach events; flags unreleased handles at window close |
| `gucs.c` | Defines all `pg_ext_memcheck.*` GUC parameters |
| `worker_harness.c` | Background worker for crash-isolated scenario execution *(Phase 2)* |
| `sql_api.c` | Implements all `ext_memcheck.*` SQL-callable functions |

---

## Regression tests

```bash
PG_CONFIG=pg_config ./test/run_tests.sh
```

---

## Known limitations

| Limitation | Detail |
|---|---|
| Not production-safe | Instruments internals not designed for runtime inspection |
| PG 15+ only | Relies on `MemoryContextData` layout introduced in PG 15 |
| Context name collisions | Named context matching can fail if two contexts share a name |
| Single-backend view | Phase 1 does not observe allocations in other backend processes |
| Nested query blind spot | `before_snapshot` is a single pointer; nested SQL (e.g. PL/pgSQL calling SQL) causes the inner `ExecutorEnd` to clear it, so the outer query is silently not analyzed |

---

## Roadmap

**Phase 1 (current):** Context leak detection, wrong-context allocation detection, shmem sentinel probing, DSM lifecycle tracking, SQL-queryable violation log, session-level control API (`begin` / `end` / `run_scenario`).

**Phase 2:** BGWorker crash harness, full stress scenario catalog (`context_reset_storm`, `concurrent_backends`, `dsm_lifecycle_check`, `wrong_context_probe`).

See the [full roadmap](https://pg-ext-memcheck.vercel.app/roadmap/) for live development status.

---

## License

[LICENSE](LICENSE)
