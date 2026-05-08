# Makefile for pg_ext_memcheck extension
EXTENSION = pg_ext_memcheck
DATA = sql/pg_ext_memcheck--0.0.1.sql

# Source files for the extension
OBJS = src/pg_ext_memcheck.o 

# Name of the shared library to be built
MODULE_big = pg_ext_memcheck 

# PGXS makefile
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)