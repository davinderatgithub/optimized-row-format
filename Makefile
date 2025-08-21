MODULE_big = optimized_row_format
OBJS = \
    $(WIN32RES) \
    optimized_row_format.o \
    orf_scan.o \
    orf_slot.o \
    orf_dml.o \
    orf_utils.o

EXTENSION = optimized_row_format
DATA = sql/optimized_row_format--1.0.sql
PGFILEDESC = "optimized_row_format - optimized row format implementation"

# REGRESS = correctness smoke known_issues
REGRESS =
REGRESS_OPTS = --inputdir=test --outputdir=test

REGRESS_PERF = performance

EXTRA_CLEAN = test/results/performance.out test/results/smoke.out

# Define a new target for performance testing
.PHONY: performance-check
performance-check: install
	../../src/test/regress/pg_regress --dbname=contrib_regression $(REGRESS_PERF) $(REGRESS_OPTS)

.PHONY: installcheck
installcheck: install
	../../src/test/regress/pg_regress --dbname=contrib_regression correctness smoke known_issues $(REGRESS_OPTS)


ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/optimized_row_format
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif