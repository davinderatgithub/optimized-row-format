MODULE_big = optimized_storage
OBJS = \
    $(WIN32RES) \
    optimized_storage.o \
    optimized_metadata.o \
    optimized_layout.o

EXTENSION = optimized_storage
DATA = optimized_storage--1.0.sql
PGFILEDESC = "optimized_storage - optimized table storage implementation"

REGRESS = optimized_storage

# Include catalog files
CATALOG_HEADERS = catalog/pg_optimized_table_metadata.h
CATALOG_DATA = catalog/pg_optimized_table_metadata_d.h

# Generation rules for catalog files
catalog/pg_optimized_table_metadata.h: catalog/pg_optimized_table_metadata.sql
	$(MAKE) -C $(top_builddir)/src/backend/catalog $@

catalog/pg_optimized_table_metadata_d.h: catalog/pg_optimized_table_metadata.sql
	$(MAKE) -C $(top_builddir)/src/backend/catalog $@

# Clean catalog files
clean:
	rm -f $(CATALOG_HEADERS) $(CATALOG_DATA)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/optimized_storage
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif