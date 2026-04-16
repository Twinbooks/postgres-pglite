#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for the timezone library

# IDENTIFICATION
#    src/timezone/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "zic - time zone compiler"
PGAPPICON = win32

subdir = src/timezone
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

# files to build into backend
OBJS = \
	localtime.o \
	pgtz.o \
	strftime.o

# files needed to build zic utility program
ZICOBJS = \
	$(WIN32RES) \
	zic.o

PGLITE_REMAP_CFLAGS = -D__PGLITE__ -Dexit=pgl_exit -Dfcntl=pgl_fcntl -Datexit=pgl_atexit -Dsetsockopt=pgl_setsockopt -Dgetsockopt=pgl_getsockopt -Dgetsockname=pgl_getsockname -Drecv=pgl_recv -Dsend=pgl_send -Dconnect=pgl_connect -Dpoll=pgl_poll -Dshmget=pgl_shmget -Dshmat=pgl_shmat -Dshmdt=pgl_shmdt -Dshmctl=pgl_shmctl -Dlongjmp=pgl_longjmp -Dsiglongjmp=pgl_siglongjmp
ZIC_CFLAGS = $(filter-out $(PGLITE_REMAP_CFLAGS),$(CFLAGS))

# we now distribute the timezone data as a single file
TZDATAFILES = $(srcdir)/data/tzdata.zi

# any custom options you might want to pass to zic while installing data files
ZIC_OPTIONS =

# use system timezone data?
ifneq (,$(with_system_tzdata))
override CPPFLAGS += '-DSYSTEMTZDIR="$(with_system_tzdata)"'
endif

include $(top_srcdir)/src/backend/common.mk

ifeq (,$(with_system_tzdata))
all: zic
endif

# We could do this test in the action section:
#	$(if $(ZIC),$(ZIC),./zic)
# but GNU make versions <= 3.78.1 or perhaps later have a bug
# that causes a segfault;  GNU make 3.81 or later fixes this.
ifeq (,$(ZIC))
ifeq ($(PORTNAME), emscripten)
ZIC= /usr/sbin/zic
else
ZIC= ./zic
endif
endif

zic.o: zic.c
	$(CC) $(ZIC_CFLAGS) $(CPPFLAGS) -c -o $@ $<

zic: $(ZICOBJS) | submake-libpgport
	$(CC) $(ZIC_CFLAGS) $(ZICOBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
ifeq (,$(with_system_tzdata))
	$(ZIC) -d '$(DESTDIR)$(datadir)/timezone' $(ZIC_OPTIONS) $(TZDATAFILES)
endif
	$(MAKE) -C tznames $@

# Note: -P code currently depends on '-b fat'.  Not worth fixing right now.
abbrevs.txt: zic $(TZDATAFILES)
	mkdir junkdir
	$(ZIC) -P -b fat -d junkdir $(TZDATAFILES) | LANG=C sort | uniq >abbrevs.txt
	rm -rf junkdir

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(datadir)'

uninstall:
ifeq (,$(with_system_tzdata))
	rm -rf '$(DESTDIR)$(datadir)/timezone'
endif
	$(MAKE) -C tznames $@

clean distclean:
	rm -f zic$(X) $(ZICOBJS) abbrevs.txt
ifeq ($(PORTNAME), emscripten)
	rm -f zic.wasm
endif
