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
- [ ] **Violation log** (`violation_log.c` / `violation_log.h`): implement `ViolationEntry` struct, in-memory ring buffer, `violation_log_write()` and `violation_log_read_all()`.  Replace bare `elog` calls in `memcheck_hooks.c` with structured violation log writes.
- [ ] **Wrong-context allocation detection**: inside the executor hooks, inspect the diff for new allocations that landed in `TopMemoryContext` or `CacheMemoryContext` and emit a `wrong_ctx_alloc` violation.
- [ ] **SQL API** (`sql/pg_ext_memcheck--0.0.1.sql`): create `memcheck` schema; implement `memcheck.begin()`, `memcheck.end()`, and the `memcheck.violations` view backed by `violation_log_read_all()`.
- [ ] **Regression tests** (`test/`): add a basic self-test suite that loads the extension, runs a known allocation pattern, and validates the violation log output.

### Phase 2 — Additions

- [ ] **Shmem sentinel probe** (`shmem_probe.c`): implement `probe_register()` and `probe_check()` to write and verify a `0xDE` sentinel byte past a declared shmem segment end.
- [ ] **DSM lifecycle tracker** (`dsm_tracker.c`): track `dsm_attach` / `dsm_detach` per backend; register an `on_proc_exit` callback to report still-attached handles as `dsm_leak` violations.
- [ ] **BGWorker crash harness** (`worker_harness.c`): launch a `BackgroundWorker` for crash-inducing tests (use-after-reset); capture non-zero exit / SIGSEGV as a confirmed bug and write to the violation log.
- [ ] **Shared violation log**: wire `shmem_startup_hook` (currently `NULL` in `pg_ext_memcheck.c`); move the violation ring buffer into shared memory so findings from all backends are queryable from a single SQL view.
- [ ] **`memcheck.run_scenario()` SQL function** and full scenario catalog (`context_reset_storm`, `tx_abort_loop`, `concurrent_backends`, `growth_benchmark`, etc.).
- [ ] **Allowlist for intentional global-context allocations**: add `allowed_contexts` parameter to `memcheck.begin()` to suppress false positives from extensions that deliberately use `TopMemoryContext`.
- [ ] Update documentation with installation steps, usage examples, and known limitations.