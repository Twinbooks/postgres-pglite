subdir = pglite/libpq-pglite
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

NAME = pqpglite
SO_MAJOR_VERSION = 5
SO_MINOR_VERSION = $(MAJORVERSION)
SHLIB_EXPORTS = libpq-pglite.exports.txt

override CPPFLAGS := -I$(top_srcdir)/src/interfaces/libpq -I$(top_builddir)/src/port -I$(top_srcdir)/src/port -I$(top_builddir)/pglite/native $(CPPFLAGS)
override CFLAGS += $(PTHREAD_CFLAGS)
override CFLAGS := $(filter-out -fvisibility=hidden,$(CFLAGS))

OBJS = libpq-pglite.o

SHLIB_LINK = -L$(top_builddir)/pglite/native -lpglite
SHLIB_LINK_INTERNAL = -L$(top_builddir)/src/common -L$(top_builddir)/src/port -lpgcommon_shlib -lpgport_shlib
SHLIB_PREREQS = submake-pglite-native submake-libpgcommon submake-libpgport

all: all-lib

include $(top_builddir)/src/Makefile.shlib

.PHONY: stage-runtime smoke psycopg2-sdk prepare-upstream-psycopg2 build-upstream-psycopg2 wheel-upstream-psycopg2-pglite smoke-upstream-psycopg2 stress-upstream-psycopg2 upstream-psycopg2-smoke

submake-pglite-native:
	$(MAKE) -C $(top_builddir)/pglite/native all

submake-libpgcommon:
	$(MAKE) -C $(top_builddir)/src/common libpgcommon_shlib.a

test-libpq-pglite.o: test-libpq-pglite.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

stage-runtime: $(shlib)
	$(MKDIR_P) '$(top_builddir)/pglite/out/native/lib'
	cp $(shlib) '$(top_builddir)/pglite/out/native/lib/'
ifneq ($(shlib), $(shlib_major))
	ln -sf $(shlib) '$(top_builddir)/pglite/out/native/lib/$(shlib_major)'
endif
ifneq ($(shlib), $(shlib_bare))
	ln -sf $(shlib) '$(top_builddir)/pglite/out/native/lib/$(shlib_bare)'
endif

test-libpq-pglite: test-libpq-pglite.o stage-runtime | $(SHLIB_PREREQS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test-libpq-pglite.o -L. -lpqpglite -L$(top_builddir)/pglite/native -lpglite -Wl,-rpath,$(CURDIR) -Wl,-rpath,$(top_builddir)/pglite/native

smoke: test-libpq-pglite
	./test-libpq-pglite

psycopg2-sdk: stage-runtime
	./materialize-psycopg2-sdk.sh

prepare-upstream-psycopg2: psycopg2-sdk
	./prepare-upstream-psycopg2.sh

build-upstream-psycopg2: prepare-upstream-psycopg2
	./build-upstream-psycopg2.sh

wheel-upstream-psycopg2-pglite: prepare-upstream-psycopg2
	./wheel-upstream-psycopg2-pglite.sh

smoke-upstream-psycopg2: build-upstream-psycopg2
	./smoke-upstream-psycopg2.sh

stress-upstream-psycopg2: build-upstream-psycopg2
	./stress-upstream-psycopg2.sh

upstream-psycopg2-smoke: smoke-upstream-psycopg2

clean: clean-lib
	rm -f test-libpq-pglite.o test-libpq-pglite
