# Makefile for buggy_pg_ext (test fixture for pg_ext_memcheck)
#
# Build with:
#   make
#   make install
#
# Requires pg_config to be on PATH (or set PG_CONFIG explicitly).

MODULE_big  = buggy_pg_ext
OBJS        = buggy_pg_ext.o
EXTENSION   = buggy_pg_ext
DATA        = buggy_pg_ext--1.0.sql
PGFILEDESC  = "buggy_pg_ext - intentionally buggy extension for pg_ext_memcheck regression testing"

PG_CONFIG   ?= pg_config
PGXS        := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
