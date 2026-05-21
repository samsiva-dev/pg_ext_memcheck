# pg_ext_memcheck

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
| `ts` | `2026-05-10 14:32:01 UTC` |
| `backend_pid` | `12345` |
| `detail` | `"Context present in post-query snapshot but not pre-query"` |
| `severity` | `warning`, `info`, `critical` |
| `check_type` | `context_leak`, `wrong_context_allocation`, `shmem_overrun` |

---

## Stress scenarios

```sql
-- Measures context growth over repeated calls — catches slow cumulative leaks
SELECT ext_memcheck.run_scenario('growth_benchmark', 100);

-- Tests memory cleanup on transaction abort
SELECT ext_memcheck.run_scenario('tx_abort_loop', 50);

SELECT ext_memcheck.flush_violations();
```

`growth_benchmark`, `tx_abort_loop`, and `shmem_sentinel_probe` are available now. Additional scenarios (`context_reset_storm`, `concurrent_backends`, `dsm_lifecycle_check`, `wrong_context_probe`) are planned for Phase 2.

---

## GUC parameters

| Parameter | Type | Default | Description |
|---|---|---|---|
| `pg_ext_memcheck.memcheck_mode` | `enum` | `all` | `all` / `executor` / `none` — controls which execution phases are hooked. |
| `pg_ext_memcheck.min_leak_bytes` | `int` | `8192` | Context growth smaller than this (bytes) is silently ignored. |

```sql
-- Focus on executor phase only (reduces noise for targeted testing)
SET pg_ext_memcheck.memcheck_mode = 'executor';

-- Disable all instrumentation (zero overhead)
SET pg_ext_memcheck.memcheck_mode = 'none';
```

---

## SQL API reference

| Function | Returns | Description |
|---|---|---|
| `ext_memcheck.begin(target_mode TEXT)` | `text` | Opens a manual test window and sets `memcheck_mode`. |
| `ext_memcheck.end()` | `TABLE(check_type, severity, detail, ts)` | Closes the window and returns violations detected. |
| `ext_memcheck.flush_violations()` | `int` | Drains the ring buffer into `violation_log`; returns count flushed. |
| `ext_memcheck.run_scenario(name TEXT, iterations INT)` | `text` | Runs a named stress scenario. |

The ring buffer is capped at 256 entries (oldest-first eviction when full). Call `flush_violations()` regularly to avoid data loss.

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

**Phase 2:** BGWorker crash harness, full stress scenario catalog.

See the [full roadmap](https://pg-ext-memcheck.vercel.app/roadmap/) for live development status.

---

## License

[LICENSE](LICENSE)
