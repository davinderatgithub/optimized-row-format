MODULE_big = optimized_row_format
OBJS = \
    $(WIN32RES) \
    optimized_row_format.o

EXTENSION = optimized_row_format
DATA = sql/optimized_row_format.sql
PGFILEDESC = "optimized_row_format - optimized row format implementation"

REGRESS = optimized_row_format

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