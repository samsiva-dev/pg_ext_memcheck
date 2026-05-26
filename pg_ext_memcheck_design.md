
# pg_ext_memcheck — Design & Planning Document

## Table of Contents

- [pg\_ext\_memcheck — Design \& Planning Document](#pg_ext_memcheck--design--planning-document)
  - [Table of Contents](#table-of-contents)
  - [1. Project Goal](#1-project-goal)
    - [The Gap Valgrind Leaves](#the-gap-valgrind-leaves)
    - [What pg\_ext\_memcheck Does](#what-pg_ext_memcheck-does)
  - [2. Relationship to Valgrind / ASan](#2-relationship-to-valgrind--asan)
  - [3. Scope — What This Tool Covers](#3-scope--what-this-tool-covers)
  - [3. Out of Scope — What This Tool Does NOT Cover](#3-out-of-scope--what-this-tool-does-not-cover)
  - [4. Threat Model — What Memory Bugs We Target](#4-threat-model--what-memory-bugs-we-target)
    - [Bug 1 — MemoryContext Leak](#bug-1--memorycontext-leak)
    - [Bug 2 — Wrong-Context Allocation](#bug-2--wrong-context-allocation)
    - [Bug 3 — Use-After-Reset (Dangling Pointer)](#bug-3--use-after-reset-dangling-pointer)
    - [Bug 4 — Shmem Segment Overrun](#bug-4--shmem-segment-overrun)
    - [Bug 5 — DSM Segment Leak](#bug-5--dsm-segment-leak)
    - [Bug 6 — Monotonic Context Growth (Bloat)](#bug-6--monotonic-context-growth-bloat)
  - [5. Architecture Design](#5-architecture-design)
    - [Key Architectural Decisions](#key-architectural-decisions)
  - [6. Module-Level Design](#6-module-level-design)
    - [6.1 `context_walker.c`](#61-context_walkerc)
    - [6.2 `shmem_probe.c`](#62-shmem_probec)
    - [6.3 `dsm_tracker.c`](#63-dsm_trackerc)
    - [6.4 `violation_log.c`](#64-violation_logc)
    - [6.5 `memcheck_hooks.c`](#65-memcheck_hooksc)
    - [6.6 `worker_harness.c`](#66-worker_harnessc)
  - [7. SQL API Design](#7-sql-api-design)
  - [8. Stress Scenario Catalog](#8-stress-scenario-catalog)
  - [9. Internal Data Structures](#9-internal-data-structures)
    - [MemoryContext Tree Diff Algorithm](#memorycontext-tree-diff-algorithm)
    - [Shmem Layout for pg\_ext\_memcheck Itself](#shmem-layout-for-pg_ext_memcheck-itself)
  - [10. Build \& Integration Plan](#10-build--integration-plan)
    - [Prerequisites](#prerequisites)
    - [Directory Layout](#directory-layout)
    - [Installation](#installation)
  - [11. MVP Scope (Phase 1)](#11-mvp-scope-phase-1)
  - [12. Phase 2 — Additions](#12-phase-2--additions)
  - [13. Known Limitations \& Hard Constraints](#13-known-limitations--hard-constraints)
  - [14. Open Questions](#14-open-questions)
  - [15. Reference — PostgreSQL Internals Used](#15-reference--postgresql-internals-used)

---

## 1. Project Goal

### The Gap Valgrind Leaves

Valgrind is the standard tool for memory analysis in C programs. When run against a PostgreSQL backend, it works — but it treats the process as **just another C program**. It sees raw `malloc` and `free`. It knows nothing about:

- **MemoryContext tree semantics** — whether a `palloc` landed in `TopMemoryContext` by mistake vs. a short-lived `PortalContext` by design
- **Context lifecycle** — whether a context was correctly deleted after use, or is silently accumulating across queries
- **Shared memory layout** — whether an extension wrote past the end of its declared `ShmemAlloc` segment
- **DSM lifecycle** — whether a `dsm_segment` was properly detached on backend exit
- **Use-after-reset** — whether an extension holds a pointer into a context that PostgreSQL later reset during normal query processing

These are not memory-unsafe bugs in the C sense — Valgrind won't flag them. But they are **logically wrong** in the PostgreSQL context model, and they cause real problems: session-level memory bloat, stale pointer dereferences, and hard-to-reproduce corruption under concurrent load.

### What pg_ext_memcheck Does

`pg_ext_memcheck` **complements Valgrind** — it does not replace it. The intended workflow is:

```
Valgrind / ASan          →   catches raw heap corruption, buffer overflows, use-after-free
pg_ext_memcheck          →   catches PostgreSQL-semantics violations: wrong context,
                              context leaks, shmem overruns, DSM leaks, use-after-reset
```

It is a PostgreSQL extension that runs **inside the backend process**, using PostgreSQL's own internal APIs (`MemoryContextData`, `ShmemAlloc`, `dsm_attach`, executor hooks) to observe and stress-test another extension's memory behaviour with full awareness of PostgreSQL's memory model.

**Target audience:** Extension authors and database engineers validating extension code. Not for production monitoring.

---

## 2. Relationship to Valgrind / ASan

This is the most important framing to understand before reading the rest of the document.

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Extension Memory Testing Stack                    │
│                                                                     │
│  ┌───────────────────────────┐  ┌──────────────────────────────┐   │
│  │     Valgrind / ASan        │  │      pg_ext_memcheck          │   │
│  │                           │  │                              │   │
│  │  • Raw heap overflow       │  │  • MemoryContext leak        │   │
│  │  • Use-after-free          │  │  • Wrong-context palloc      │   │
│  │  • Uninitialized reads     │  │  • Shmem overrun             │   │
│  │  • Double free             │  │  • DSM lifecycle             │   │
│  │                           │  │  • Use-after-reset           │   │
│  │  Sees: raw malloc/free     │  │  • Context bloat             │   │
│  │  Blind to: PG context tree │  │                              │   │
│  │                           │  │  Sees: PG context semantics  │   │
│  │                           │  │  Blind to: raw heap bugs     │   │
│  └───────────────────────────┘  └──────────────────────────────┘   │
│                                                                     │
│              Use both. They cover different bug classes.            │
└─────────────────────────────────────────────────────────────────────┘
```

**Why Valgrind alone is not enough for extensions:**

Valgrind instruments memory at the `malloc/free` level. PostgreSQL's `palloc` sits on top of `malloc` but adds a full context abstraction layer. From Valgrind's perspective, allocating 1000 bytes in `TopMemoryContext` and allocating 1000 bytes in a short-lived `PortalContext` look identical — both are valid heap allocations. The semantic error (wrong context, no cleanup path) is invisible to it.

Similarly, `MemoryContextReset()` does not call `free()` on individual allocations — it bulk-resets the context's internal block list. A pointer into a reset context is not a use-after-free in the `malloc` sense. Valgrind will not flag it. But dereferencing it is still a bug.

**The intended workflow:**

```
1. Run Valgrind         →  fix raw heap bugs first
2. Run pg_ext_memcheck  →  fix PG-semantics bugs second
3. Ship
```

---

## 3. Scope — What This Tool Covers

The table below also shows what Valgrind covers for the same area, to make the complementary positioning concrete.

| Area | Covered | Valgrind covers this? | Notes |
|---|---|---|---|
| MemoryContext leak detection | ✅ | ❌ Sees palloc as malloc — no context semantics | Snapshot + diff of context tree |
| Wrong-context allocation detection | ✅ | ❌ No concept of context hierarchy | Allocations landing in TopMemoryContext / CacheMemoryContext |
| Shmem boundary overrun detection | ✅ | ⚠️ Only if it causes a raw heap write outside mapped memory | Sentinel byte probe past declared segment |
| DSM lifecycle tracking | ✅ | ❌ Does not track dsm_segment handles | Attach/detach tracking per backend exit |
| Use-after-reset simulation | ✅ | ⚠️ Only if reset causes actual deallocation (not always) | Force reset of target context, re-invoke extension |
| Context bloat over repeated calls | ✅ | ❌ No | Measure size growth across N invocations |
| Named context identification | ✅ | ❌ No | Match by context name string in tree walk |
| SQL-level test harness | ✅ | ❌ No | `ext_memcheck.begin()` / `ext_memcheck.end()` / `ext_memcheck.run_scenario()` |
| Background worker crash isolation | ✅ | ❌ No | Fork worker to isolate crash-inducing tests |
| Raw heap buffer overflow | ❌ | ✅ Primary strength — use Valgrind/ASan | Out of scope by design |
| Use-after-free (raw malloc/free) | ❌ | ✅ Use Valgrind/ASan | Out of scope by design |
| Uninitialized memory reads | ❌ | ✅ Use Valgrind | Out of scope by design |

---

## 3. Out of Scope — What This Tool Does NOT Cover

These are explicitly excluded. Do not design for them.

| Area | Reason Excluded |
|---|---|
| Raw C heap corruption outside palloc | **This is Valgrind/ASan territory.** We operate only inside PG's allocator. Use both tools together. |
| Uninitialized memory reads | **This is Valgrind territory** (`--track-origins=yes`). Not PG-context-specific. |
| Production monitoring / always-on mode | Sentinel writes and context hooks add measurable overhead. Requires explicit `ext_memcheck.begin()`. |
| Multi-node / Citus / distributed shard memory | Coordinator-only scope; cross-node memory tracking requires distributed tracing infrastructure |
| GPU / accelerator memory | Out of PostgreSQL backend scope entirely |
| Extension ABI compatibility checking | Separate concern; that's for pg_upgrade / extension versioning tooling |
| VACUUM / autovacuum worker memory | These run in separate workers; testing them requires a separate harness |
| Memory accounting for JIT (LLVM) allocations | LLVM uses its own allocator; not visible through palloc context tree |
| Detecting bugs in PostgreSQL core itself | We assume core is correct; we are testing the extension layer only |
| Automated CI gate | That's the user's pipeline responsibility — no opinion on CI tooling |

---

## 4. Threat Model — What Memory Bugs We Target

These are the six concrete bug classes the tool is designed to detect.

### Bug 1 — MemoryContext Leak
Extension allocates a child context in `_PG_init` or an executor hook, but never calls `MemoryContextDelete()`. The context accumulates across queries.

**Signal:** Context tree diff shows new named contexts not present before the test window.

---

### Bug 2 — Wrong-Context Allocation
Extension calls `palloc()` while `CurrentMemoryContext` is `TopMemoryContext` or `CacheMemoryContext` — either by mistake or due to a missing context switch. The allocation lives for the lifetime of the process.

**Signal:** During the test window, any `palloc` landing in a "global" context that was not already there.

---

### Bug 3 — Use-After-Reset (Dangling Pointer)
Extension stores a pointer into a context (e.g., `PortalContext`, `QueryContext`), which is later reset by PostgreSQL's normal flow. The extension still holds and dereferences the pointer.

**Signal:** Forced `MemoryContextReset()` followed by re-invocation causes SIGSEGV or corrupt output. (Caught via forked worker; crash = confirmed bug.)

---

### Bug 4 — Shmem Segment Overrun
Extension requests N bytes via `RequestAddinShmemSpace()` in `_PG_init`, but writes past the end of the segment during normal operation.

**Signal:** Sentinel byte written just past `base + declared_size` is overwritten after workload run.

---

### Bug 5 — DSM Segment Leak
Extension attaches a DSM segment but does not detach it on backend exit (missing `on_dsm_detach` callback or missing `dsm_detach()` call in cleanup code).

**Signal:** DSM tracker reports segment handles still attached when `proc_exit` fires.

---

### Bug 6 — Monotonic Context Growth (Bloat)
Extension does not leak a context per se, but reuses a context without ever resetting it. Over N invocations, the context grows without bound.

**Signal:** Context size measurement after 1, 10, 100, 1000 invocations — linear or superlinear growth.

---

## 5. Architecture Design

```
┌─────────────────────────────────────────────────────────────┐
│                     pg_ext_memcheck Extension                │
│                                                             │
│  ┌─────────────┐  ┌──────────────┐  ┌───────────────────┐  │
│  │ context_    │  │ shmem_       │  │ dsm_              │  │
│  │ walker.c    │  │ probe.c      │  │ tracker.c         │  │
│  │             │  │              │  │                   │  │
│  │ Walks the   │  │ Writes       │  │ Hooks attach/     │  │
│  │ MemoryCtx   │  │ sentinel     │  │ detach; reports   │  │
│  │ tree via    │  │ past shmem   │  │ leaks on exit     │  │
│  │ internal    │  │ segment end  │  │                   │  │
│  │ MemoryCtx   │  │              │  │                   │  │
│  │ Data API    │  │              │  │                   │  │
│  └──────┬──────┘  └──────┬───────┘  └────────┬──────────┘  │
│         │                │                    │             │
│  ┌──────▼────────────────▼────────────────────▼──────────┐  │
│  │                 violation_log.c                        │  │
│  │   Shared ring buffer — check_type, severity, detail   │  │
│  └──────────────────────────┬────────────────────────────┘  │
│                             │                               │
│  ┌──────────────────────────▼────────────────────────────┐  │
│  │               memcheck_hooks.c                         │  │
│  │   ExecutorStart / ExecutorEnd hooks wrap test window   │  │
│  └──────────────────────────┬────────────────────────────┘  │
│                             │                               │
│  ┌──────────────────────────▼────────────────────────────┐  │
│  │               worker_harness.c                         │  │
│  │   BackgroundWorker fork for crash-safe tests           │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
                             │
                    SQL Interface Layer
              ┌──────────────┴──────────────┐
              │   ext_memcheck schema            │
              │   begin() / end()            │
              │   run_scenario()             │
              │   report view                │
              └──────────────────────────────┘
```

### Key Architectural Decisions

**Decision 1: C extension, not external tool**  
Context tree addresses, shmem metadata, and DSM handles are only accessible from inside the backend. An external tool (like a wrapper script or `gdb` script) cannot see `MemoryContextData` layout without binary inspection.

**Decision 2: Forked BackgroundWorker for crash-inducing tests**  
Use-after-reset tests *should* crash if the bug is real. Running them in the calling backend would kill the session. A BGWorker with a known timeout lets us detect SIGSEGV cleanly.

**Decision 3: Named context matching, not address matching**  
Extension contexts are identified by the name string passed to `AllocSetContextCreate`. Addresses change across restarts; names are stable across runs and across PG versions.

**Decision 4: Ring buffer for violation log**  
Multiple backends can run tests concurrently. A shared ring buffer in shmem (fixed size, overwrite-oldest policy) lets all findings be queryable via a single SQL view regardless of which backend ran the test.

**Decision 5: Sentinel byte, not full guard pages**  
Full guard pages require `mprotect` and are not portable across all PG build environments. A sentinel byte is simpler, portable, and sufficient to detect overruns in typical extension code.

---

## 6. Module-Level Design

### 6.1 `context_walker.c`

Responsibility: Walk the `MemoryContextData` tree, produce a snapshot of all live contexts with name, parent, total allocated bytes, and total free bytes.

**Key PG internals used:**
- `MemoryContextData` struct (`utils/palloc.h`)
- `MemoryContextStats()` — outputs stats to a `MemoryContextCounters` struct
- `firstchild` / `nextchild` pointers on `MemoryContextData`

**Snapshot struct:**
```c
typedef struct CtxSnapshot {
    char   name[NAMEDATALEN];
    Size   totalAllocated;
    Size   totalFree;
    int    depth;           /* depth in tree */
    uint32 parentHash;      /* hash of parent name+depth for diff */
} CtxSnapshot;

typedef struct CtxTree {
    CtxSnapshot *entries;
    int          count;
    int          capacity;
} CtxTree;
```

**Operations:**
- `snapshot_context_tree(MemoryContext root) → CtxTree*`
- `diff_context_trees(CtxTree *before, CtxTree *after) → CtxDiff*`
- `free_ctx_tree(CtxTree *tree)`

---

### 6.2 `shmem_probe.c`

Responsibility: Write a sentinel byte just past the end of the extension's declared shmem segment, verify it is intact after the workload.

**Key PG internals used:**
- `ShmemAlloc()` base address tracking (requires cooperation — extension must register its base pointer, or we probe the known `ShmemIndex` hash)
- `ShmemInitStruct()` to locate named shmem segments by name

**Operations:**
- `probe_register(const char *seg_name, Size declared_size)` — write sentinel
- `probe_check(const char *seg_name) → bool` — return false if sentinel overwritten
- Sentinel value: `0xDE` (unlikely in normal data; simple to spot in core dumps)

**Limitation:** Only works for segments registered via `ShmemInitStruct` with a known name. Unnamed or anonymous allocations cannot be probed without deeper patching.

---

### 6.3 `dsm_tracker.c`

Responsibility: Track DSM segment attach/detach events per backend. Report any segment still attached at `proc_exit`.

**Key PG internals used:**
- `dsm_attach()` — wrap via hook or wrap the SQL path
- `on_proc_exit()` callback registered at extension load time
- `dsm_segment` handle list stored in local memory

**Operations:**
- `dsm_track_attach(dsm_segment *seg)` — add to tracked list
- `dsm_track_detach(dsm_segment *seg)` — remove from tracked list
- `dsm_proc_exit_check(int code, Datum arg)` — registered via `on_proc_exit`; logs any remaining handles

**Note:** This only works if the extension under test uses standard `dsm_attach/detach` paths. Extensions using `shm_open` directly bypass this entirely (and that is fine — we document it as a known gap).

---

### 6.4 `violation_log.c`

Responsibility: Write findings to a shared ring buffer readable via SQL.

**Ring buffer layout (in shmem):**
```c
#define MEMCHECK_MAX_VIOLATIONS 2048

typedef struct ViolationEntry {
    TimestampTz  ts;
    int          backend_pid;
    char         check_type[32];   /* "context_leak", "wrong_ctx_alloc", etc. */
    char         severity[16];     /* "ERROR", "WARNING", "INFO" */
    char         detail[256];
    char         source_lib[64];   /* basename of the .so that triggered the violation */
} ViolationEntry;

typedef struct ViolationLog {
    LWLock       lock;
    int          head;
    int          count;
    ViolationEntry entries[MEMCHECK_MAX_VIOLATIONS];
} ViolationLog;
```

**Operations:**
- `violation_log_write(check_type, severity, detail)`
- `violation_log_read_all() → ViolationEntry[]` — called by SQL function backing the view

---

### 6.5 `memcheck_hooks.c`

Responsibility: Install/remove ExecutorStart and ExecutorEnd hooks to bracket the test window.

```c
/* Saved previous hooks for chaining */
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorEnd_hook_type   prev_ExecutorEnd   = NULL;

/* Test session state */
static bool    memcheck_active  = false;
static char    target_ext[NAMEDATALEN];
static CtxTree *snapshot_before = NULL;
```

On `ExecutorStart`: if `memcheck_active` and the query is not a `ext_memcheck` internal query, take the before-snapshot.  
On `ExecutorEnd`: take after-snapshot, run diff, write violations.

---

### 6.6 `worker_harness.c`

Responsibility: Launch a `BackgroundWorker` to run crash-inducing tests (use-after-reset, OOM simulation). Collect exit status. Report SIGSEGV or abnormal exit as a confirmed bug.

**BGWorker approach:**
- Worker receives test scenario name and target extension name via a small shmem slot
- Worker runs the scenario, writes to the violation log if clean
- Postmaster reaps the worker; harness reads exit code
- Exit code != 0 → confirmed crash → write `"use_after_reset": ERROR` to violation log

---

## 7. SQL API Design

All functions live in the `ext_memcheck` schema.

```sql
-- Start a test session targeting an extension by context name pattern
SELECT ext_memcheck.begin(
    ext_context_pattern TEXT DEFAULT '',  -- e.g. 'MyExtCtx%'
    options             JSONB DEFAULT NULL -- optional: {"track_shmem": true, "track_dsm": true}
);

-- End the test session and return all findings
SELECT * FROM ext_memcheck.end();
-- Returns: check_type TEXT, severity TEXT, detail TEXT, ts TIMESTAMPTZ, source_lib TEXT

-- Run a named stress scenario
SELECT ext_memcheck.run_scenario(
    scenario_name TEXT,                   -- e.g. 'growth_benchmark', 'tx_abort_loop'
    iterations    INTEGER DEFAULT 100,
    workload      TEXT    DEFAULT 'SELECT 1'
);

-- Flush the in-memory violation ring buffer into violation_log; returns rows inserted
SELECT ext_memcheck.flush_violations();

-- Query the persisted violation log (across all sessions)
SELECT * FROM ext_memcheck.violation_log
ORDER BY ts DESC
LIMIT 100;

-- Clear the violation log
SELECT ext_memcheck.clear_violations();

-- List available scenarios
SELECT * FROM ext_memcheck.scenarios;

-- DSM segment tracking
SELECT * FROM ext_memcheck.dsm_tracking();
SELECT ext_memcheck.track_dsm_handle(handle BIGINT);
SELECT ext_memcheck.clear_dsm_tracking();

-- Shared memory sentinel probe
SELECT ext_memcheck.register_shmem_probe(seg_name TEXT, allocated_size BIGINT);
SELECT ext_memcheck.clear_shmem_registry();
```

**`ext_memcheck.violation_log` table columns:**

| Column | Type | Notes |
|---|---|---|
| `id` | SERIAL | Primary key |
| `ts` | TIMESTAMPTZ | When the violation was logged |
| `backend_pid` | INT | Which backend ran the test |
| `check_type` | TEXT | `context_leak`, `wrong_ctx_alloc`, `shmem_overrun`, `dsm_leak`, `ctx_bloat` |
| `severity` | TEXT | `ERROR`, `WARNING`, `INFO` |
| `detail` | TEXT | Human-readable detail |
| `source_lib` | TEXT | Extension library that triggered the violation |

**`ext_memcheck.dsm_tracking()` return columns:**

| Column | Type | Notes |
|---|---|---|
| `segid` | BIGINT | DSM segment handle |
| `backend_pid` | INT | Backend that attached the segment |
| `attach_at` | TIMESTAMPTZ | When the segment was attached |
| `size_bytes` | BIGINT | Segment size |
| `detached` | BOOLEAN | Whether the segment has been detached |

---

## 8. Stress Scenario Catalog

| Scenario Name | What It Does | Target Bug Class |
|---|---|---|
| `context_reset_storm` | Calls `MemoryContextReset` on portal context 100x per query, then invokes extension | Use-after-reset |
| `tx_abort_loop` | Runs extension inside a transaction, aborts 1000x | Context leak on error path |
| `concurrent_backends` | N parallel backends all hit extension shmem simultaneously | Shmem overrun, missing locks |
| `cursor_leak` | Opens a cursor via extension, holds it across 10 transactions without closing | DSM / portal context leak |
| `oom_simulation` | Overrides palloc to fail after N bytes; checks extension error handling | Wrong-context fallback, crash on OOM |
| `growth_benchmark` | Calls extension 1, 10, 100, 1000 times; records context size at each step | Monotonic context bloat |
| `cold_warm_cold` | Run extension, idle for idle_seconds, run again — tests cache context durability | CacheMemoryContext misuse |

---

## 9. Internal Data Structures

### MemoryContext Tree Diff Algorithm

The diff is name-and-depth based, not pointer based (pointers are unstable across calls).

```
For each entry E in after-snapshot:
    Look for entry in before-snapshot with same (name, depth, parentHash)
    If not found → NEW context → potential leak
    If found → compare size delta
        If delta > threshold → potential bloat

For each entry E in before-snapshot not found in after → deleted context (expected cleanup)
```

The threshold for "bloat" is configurable via the `pg_ext_memcheck.bloat_min_bytes` GUC (default: 8192 — 8 KiB; growth below this floor is silently filtered to suppress noise from normal PG core context churn). The matching per-query leak threshold is `pg_ext_memcheck.min_leak_bytes`, also defaulting to 8 KiB. Reportable growth is classified `INFO` / `WARNING` / `ERROR` by magnitude (see severity thresholds in README).

---

### Shmem Layout for pg_ext_memcheck Itself

```
pg_ext_memcheck shmem segment:
├── ViolationLog        (2048 entries × ~376 bytes = ~770 KB)
├── DsmTrackerState     (up to 128 segments × ~32 bytes each = ~4 KB)
├── ProbeRegistry       (up to 32 probed segments × ~88 bytes each = ~3 KB)
└── SentinelTest        (10-byte test segment for register_shmem_probe regress)

Total: ~780 KB shmem (WorkerSlot deferred with worker_harness.c to Phase 2)
```

---

## 10. Build & Integration Plan

### Prerequisites
- PostgreSQL source tree (for headers: `memutils.h`, `dsm.h`, `shmem.h`, `executor/executor.h`)
- PG version: 15+ (earlier versions have different `MemoryContextData` layout)
- Build: standard `pgxs` Makefile

### Directory Layout
```
pg_ext_memcheck/
├── Makefile
├── pg_ext_memcheck.control
├── src/
│   ├── pg_ext_memcheck.c       -- _PG_init, shmem request, hook registration
│   ├── context_walker.c
│   ├── shmem_probe.c
│   ├── dsm_tracker.c
│   ├── violation_log.c
│   ├── memcheck_hooks.c
│   └── worker_harness.c
├── include/
│   └── pg_ext_memcheck.h
├── sql/
│   ├── pg_ext_memcheck--1.0.sql
│   └── scenarios/
│       ├── context_reset_storm.sql
│       ├── tx_abort_loop.sql
│       └── ...
└── test/
    ├── regress/
    │   └── memcheck_basic.sql
    └── expected/
        └── memcheck_basic.out
```

### Installation
```bash
make PG_CONFIG=/path/to/pg_config
make install

# In psql:
CREATE EXTENSION pg_ext_memcheck;
# Requires: shared_preload_libraries = 'pg_ext_memcheck'
```

---

## 11. MVP Scope (Phase 1 — shipped in 0.1.0 Beta)

The MVP set defined for the first working version, all delivered:

| Feature | Module | Status |
|---|---|---|
| Context tree snapshot + diff | `context_walker.c` | ✅ Shipped |
| `ext_memcheck.begin()` / `ext_memcheck.end()` SQL API | `sql_api.c` + SQL | ✅ Shipped |
| Wrong-context allocation detection | `memcheck_hooks.c` | ✅ Shipped |
| Monotonic context-bloat detection (Bug 6) with linear / superlinear shape classification at log-spaced checkpoints; severity bumped one rung when growth is superlinear | `sql_api.c` (`analyze_bloat`), driven through the `growth_benchmark` scenario | ✅ Shipped |
| Regress self-test (`pg_ext_memcheck` checks itself) | `test/sql/`, `test/expected/` | ✅ Shipped (16 cases + buggy fixture) |
| Violation log | `violation_log.c` | ✅ Shipped — promoted directly to the shared-memory ring buffer originally planned for Phase 2, so multi-backend visibility is in 0.1.0 Beta |

### Phase 2 features pulled forward into 0.1.0 Beta

These design §12 items were implemented early and ship in 0.1.0 Beta:

- Shmem sentinel probe (`shmem_probe.c`) + `register_shmem_probe()` / `probe_check()` / `clear_shmem_registry()` SQL API
- DSM lifecycle tracker (`dsm_tracker.c`) + `track_dsm_handle()` / `dsm_tracking()` / `clear_dsm_tracking()` SQL API and `on_proc_exit` leak safety-net
- `ext_memcheck.run_scenario()` with the scenarios `growth_benchmark` (host for Phase 1 bloat detection above), `tx_abort_loop`, `shmem_sentinel_probe`, `wrong_context_probe`
- Shared (multi-backend) violation log via `LWLock`-protected shmem ring; per-session draining in `end()` via (`backend_pid`, `ts >= begin time`)
- Source-library attribution (`source_lib` column) resolved through `dladdr()` against the active hook chain

---

## 12. Phase 2 — Remaining work for the next release

Items still outstanding after 0.1.0 Beta:

- BGWorker crash isolation (`worker_harness.c` — stub only today; needed for Bug 3 / use-after-reset)
- Remaining stress scenarios from §8 not yet implemented: `context_reset_storm`, `concurrent_backends`, `cursor_leak`, `oom_simulation`, `cold_warm_cold`
- `growth_benchmark` scenario with chart output
- PG17 / PG18 AIO compatibility layer (AIO uses different context patterns)
- Nested-query stack for `before_snapshot` so outer-query analysis is not dropped when a nested SQL invocation closes its own executor window
- `all`-mode coverage for cached/prepared statements (extended protocol skips `planner_hook`) and utility statements (bypass both hooks)

---

## 13. Known Limitations & Hard Constraints

| Limitation | Impact | Workaround |
|---|---|---|
| Cannot detect bugs in `shm_open`-based shmem (non-PG allocator) | DSM tracker blind spot | Document; advise using `ShmemAlloc` for testability |
| Context name collisions possible if extension uses generic names | False positives in diff | Use specific, unique context names in extensions under test |
| `MemoryContextData` layout is PG-version-specific | Must test per major version | Compile with `PG_CONFIG` of the target PG version |
| Sentinel probe requires declared segment size from extension author | Cannot auto-discover | Provide `probe_register()` SQL hook for extension authors to call |
| LLVM JIT allocations invisible | JIT-heavy workloads have incomplete picture | Known gap; document it |
| Not safe to run in production | Overhead + sentinel writes | `pg_ext_memcheck.enabled = off` by default; explicit `begin()` required |

---

## 14. Open Questions

1. **How to handle extensions that intentionally use `TopMemoryContext`** (e.g., caching data across queries)?  
   → Need an allowlist mechanism: `ext_memcheck.begin(..., allowed_contexts := ARRAY['TopMemoryContext'])`.

2. **Should `ext_memcheck.end()` auto-rollback the test session if it detects errors?**  
   → Probably yes — prevent corrupt state from leaking into the user's session.

3. **PG18 AIO** uses `IoMethodOps` and async context patterns. Does the context walker handle async I/O contexts correctly?  
   → Needs investigation once PG18 async context layout is finalized.

4. **Should the tool ship with a test extension** (`ext_memcheck_test_ext`) that has known bugs for regression testing?  
   → Strongly preferred. A deliberately buggy extension lets us write deterministic regress tests.

5. **`MemoryContextStats()` vs manual tree walk?**  
   → `MemoryContextStats()` is a convenience wrapper. For diff, we need the raw tree walk via `firstchild`/`nextchild`. Use both: stats for sizes, tree walk for structure.

---

## 15. Reference — PostgreSQL Internals Used

| Internal | Header | Usage |
|---|---|---|
| `MemoryContextData` | `utils/palloc.h` | Tree walk, size stats |
| `MemoryContextStats()` | `utils/palloc.h` | Aggregate size per context |
| `AllocSetContextCreate()` | `utils/memutils.h` | Context creation (for test contexts) |
| `MemoryContextReset()` | `utils/palloc.h` | Forced reset in stress scenarios |
| `MemoryContextDelete()` | `utils/palloc.h` | Cleanup |
| `ShmemAlloc()` | `storage/shmem.h` | Shmem allocation |
| `ShmemInitStruct()` | `storage/shmem.h` | Named shmem segment lookup |
| `RequestAddinShmemSpace()` | `storage/shmem.h` | Declare shmem need in `_PG_init` |
| `dsm_attach()` / `dsm_detach()` | `storage/dsm.h` | DSM lifecycle |
| `on_proc_exit()` | `storage/ipc.h` | Exit callbacks for cleanup checks |
| `ExecutorStart_hook` | `executor/executor.h` | Test window bracketing |
| `ExecutorEnd_hook` | `executor/executor.h` | Test window close + analysis |
| `BackgroundWorker` | `postmaster/bgworker.h` | Crash-safe test isolation |
| `LWLock` | `storage/lwlock.h` | Violation log concurrency |

---

*Document version 0.1 — Phase 1 MVP planning only. Phase 2 items subject to revision based on findings during Phase 1 implementation.*
