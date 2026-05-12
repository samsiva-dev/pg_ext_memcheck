# pg_ext_memcheck
A PostgreSQL extension that instruments and stress-tests another extension's memory behaviour from inside the backend process.

## To-Do

### Phase 1 — MVP

- [x] Extension scaffold: `_PG_init` / `_PG_fini`, `Makefile`, `.control` file — extension loads into PostgreSQL.
- [x] Custom GUC: `pg_ext_memcheck.memcheck_mode` (ALL / EXECUTOR / NONE) defined in `gucs.c`.
- [x] Executor hooks: `ExecutorStart_hook` and `ExecutorEnd_hook` installed and properly chained in `memcheck_hooks.c`.
- [x] Context tree snapshot (`snapshot_context_tree`) and diff (`diff_context_trees`) implemented in `context_walker.c`.
- [x] Basic memory-delta logging in executor hooks (elog output of context diffs).
- [x] All module headers defined (`context_walker.h`, `gucs.h`, `memcheck_hooks.h`, `enums.h`).
- [x] **Violation log** (`violation_log.c` / `violation_log.h`): `ViolationEntry` struct, shared-memory ring buffer (`ViolationLog`), `violation_log_write()`, `violation_log_read_all()`, and `violation_log_flush()` (SQL-callable) all implemented. Bare `elog` calls in `memcheck_hooks.c` replaced with structured `violation_log_write()` calls via `analyze_and_log_diff()`.
- [ ] **Wrong-context allocation detection**: inside the executor hooks, inspect the diff for new allocations that landed in `TopMemoryContext` or `CacheMemoryContext` and emit a `wrong_ctx_alloc` violation.
- [x] **SQL API** (`sql/pg_ext_memcheck--0.0.1.sql`): schema `ext_memcheck`, table `ext_memcheck.violation_log`, `ext_memcheck.flush_violations()`, `ext_memcheck.begin()`, and `ext_memcheck.end()` all implemented. Still needed: `violations` view backed by `violation_log_read_all()`.
- [ ] **`violations` view**: SQL view exposing `violation_log_read_all()` results for direct `SELECT`.
- [ ] **`memcheck.run_scenario()` implementation**: function stub exists and is SQL-callable, but scenario bodies (`context_reset_storm`, `tx_abort_loop`, etc.) are not yet implemented.
- [ ] **Regression tests** (`test/`): add a basic self-test suite that loads the extension, runs a known allocation pattern, and validates the violation log output.

### Phase 2 — Additions

- [ ] **Shmem sentinel probe** (`shmem_probe.c`): implement `probe_register()` and `probe_check()` to write and verify a `0xDE` sentinel byte past a declared shmem segment end.
- [ ] **DSM lifecycle tracker** (`dsm_tracker.c`): track `dsm_attach` / `dsm_detach` per backend; register an `on_proc_exit` callback to report still-attached handles as `dsm_leak` violations.
- [ ] **BGWorker crash harness** (`worker_harness.c`): launch a `BackgroundWorker` for crash-inducing tests (use-after-reset); capture non-zero exit / SIGSEGV as a confirmed bug and write to the violation log.
- [x] **Shared violation log**: `shmem_startup_hook` wired via `memcheck_shmem_startup`; ring buffer allocated in shared memory with `ShmemInitStruct`; findings queryable from SQL via `ext_memcheck.flush_violations()`.
- [ ] **`memcheck.run_scenario()` full scenario catalog**: SQL function stub exists; scenario bodies (`context_reset_storm`, `tx_abort_loop`, `concurrent_backends`, `growth_benchmark`, etc.) not yet implemented.
- [ ] **Allowlist for intentional global-context allocations**: add `allowed_contexts` parameter to `memcheck.begin()` to suppress false positives from extensions that deliberately use `TopMemoryContext`.
- [ ] Update documentation with installation steps, usage examples, and known limitations.