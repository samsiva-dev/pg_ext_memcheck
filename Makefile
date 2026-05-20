# Makefile for pg_ext_memcheck extension
EXTENSION = pg_ext_memcheck
DATA = sql/pg_ext_memcheck--0.0.1.sql

# Source files for the extension
OBJS = src/pg_ext_memcheck.o \
       src/gucs.o \
       src/memcheck_hooks.o \
       src/context_walker.o \
       src/dsm_tracker.o \
       src/shmem_probe.o \
       src/violation_log.o \
       src/worker_harness.o \
       src/sql_api.o

# Name of the shared library to be built
MODULE_big = pg_ext_memcheck

# Ensure PGXS 'all' remains the default goal — must be before any explicit targets
.DEFAULT_GOAL := all

# Regression tests (pg_regress via PGXS installcheck)
REGRESS = \
	00_setup \
	01_gucs \
	02_session_lifecycle \
	03_violation_log \
	04_scenario_growth_benchmark \
	05_scenario_tx_abort_loop \
	06_scenario_unknown \
	07_executor_hook_mode \
	08_ring_buffer_overflow \
	09_flush_clears_buffer \
	10_min_leak_bytes_threshold \
	11_violation_log_schema \
	12_idempotent_install \
	13_shmem_sentinel_probe

REGRESS_OPTS = --inputdir=test --outputdir=test

# Self-contained regression target: spins up a temp cluster, runs all tests,
# then tears down the cluster — no pre-existing server required.
regress:
	@bash test/run_tests.sh

.PHONY: regress

# PGXS makefile
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)