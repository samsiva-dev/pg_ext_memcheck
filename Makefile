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
       src/worker_harness.o

# Name of the shared library to be built
MODULE_big = pg_ext_memcheck 

# PGXS makefile
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)