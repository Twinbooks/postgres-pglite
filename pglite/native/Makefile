subdir = pglite/native
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

NAME = pglite
SO_MAJOR_VERSION = 0
SO_MINOR_VERSION = 1
PGLITE_EXPORTS ?= libpglite.exports.txt
SHLIB_EXPORTS = $(PGLITE_EXPORTS)
SHLIB_LINK = $(filter-out -lpgport -lpgcommon, $(LIBS)) $(LDAP_LIBS_BE) $(ICU_LIBS) $(top_builddir)/src/common/logging.o $(top_builddir)/src/fe_utils/option_utils.o -L$(top_builddir)/src/interfaces/libpq -lpq
SHLIB_LINK_INTERNAL =
BACKEND_OBJFILES = $(top_builddir)/src/backend/objfiles.txt
BACKEND_OBJS = $(addprefix $(top_builddir)/,$(shell test -f $(BACKEND_OBJFILES) && tr ' ' '\n' < $(BACKEND_OBJFILES) | grep -v 'objfiles.txt$$'))
PGLITE_REMAP_CFLAGS = -Dexit=pgl_exit -Dfcntl=pgl_fcntl -Datexit=pgl_atexit -Dsetsockopt=pgl_setsockopt -Dgetsockopt=pgl_getsockopt -Dgetsockname=pgl_getsockname -Drecv=pgl_recv -Dsend=pgl_send -Dconnect=pgl_connect -Dpoll=pgl_poll -Dshmget=pgl_shmget -Dshmat=pgl_shmat -Dshmdt=pgl_shmdt -Dshmctl=pgl_shmctl -Dlongjmp=pgl_longjmp -Dsiglongjmp=pgl_siglongjmp
PGLITE_BUILD_CFLAGS ?= $(CFLAGS)
PGLITEC_CFLAGS = $(filter-out $(PGLITE_REMAP_CFLAGS),$(PGLITE_BUILD_CFLAGS))
PGLITE_FRONTEND_CFLAGS = $(filter-out $(PGLITE_REMAP_CFLAGS),$(PGLITE_BUILD_CFLAGS))
STRINGINFO_REMAP_CFLAGS = -DmakeStringInfo=pgl_makeStringInfo -DinitStringInfo=pgl_initStringInfo -DresetStringInfo=pgl_resetStringInfo -DappendStringInfo=pgl_appendStringInfo -DappendStringInfoVA=pgl_appendStringInfoVA -DappendStringInfoString=pgl_appendStringInfoString -DappendStringInfoChar=pgl_appendStringInfoChar -DappendStringInfoSpaces=pgl_appendStringInfoSpaces -DappendBinaryStringInfo=pgl_appendBinaryStringInfo -DappendBinaryStringInfoNT=pgl_appendBinaryStringInfoNT -DenlargeStringInfo=pgl_enlargeStringInfo -DdestroyStringInfo=pgl_destroyStringInfo -Dpalloc=pg_malloc -Dpfree=pg_free -Drepalloc=pg_realloc -Dpvsnprintf=pgl_pvsnprintf
INITDB_CPPFLAGS = -DFRONTEND -DUSE_PRIVATE_ENCODING_FUNCS -I$(libpq_srcdir) -I$(top_srcdir)/src/timezone $(ICU_CFLAGS)
INITDB_REMAP_CFLAGS = $(PGLITE_REMAP_CFLAGS) $(STRINGINFO_REMAP_CFLAGS) -Dsystem=pgl_system -Dpopen=pgl_popen -Dpclose=pgl_pclose -Dgeteuid=pgl_geteuid -Dgetuid=pgl_getuid -Dgetpwuid=pgl_getpwuid -Dpalloc=pg_malloc -Dpalloc0=pg_malloc0 -Dpalloc_extended=pg_malloc_extended -Dpfree=pg_free -Dpstrdup=pg_strdup -Dpnstrdup=pgl_pnstrdup -Drepalloc=pg_realloc -Dpsprintf=pgl_psprintf -Dpvsnprintf=pgl_pvsnprintf -Dmain=pglite_initdb_main
INITDB_FINDTIMEZONE_CPPFLAGS = $(INITDB_CPPFLAGS) -Dpg_open_tzfile=pglite_pg_open_tzfile
FRONTEND_OBJS = pglite_stringinfo.o
INITDB_OBJS = pglite_initdb.o pglite_findtimezone.o
OBJS = libpglite.o $(FRONTEND_OBJS) $(INITDB_OBJS) $(top_builddir)/pglite/src/pglitec/pglitec.o $(BACKEND_OBJS) $(top_builddir)/src/common/libpgcommon_srv.a $(top_builddir)/src/port/libpgport_srv.a

SHLIB_LINK := $(filter-out -lreadline -ledit -ltermcap -lncurses -lcurses, $(SHLIB_LINK))

ifeq ($(with_systemd),yes)
SHLIB_LINK += -lsystemd
endif

all: all-lib

include $(top_builddir)/src/Makefile.shlib

libpglite.o: libpglite.c libpglite.h $(top_builddir)/pglite/src/pglitec/pglitec.h
	$(CC) $(PGLITE_BUILD_CFLAGS) $(CFLAGS_SL) $(CPPFLAGS) -c $< -o $@

pglite_stringinfo.o: $(top_srcdir)/src/common/stringinfo.c $(top_builddir)/src/Makefile.global
	$(CC) $(PGLITE_FRONTEND_CFLAGS) $(STRINGINFO_REMAP_CFLAGS) $(CFLAGS_SL) $(CPPFLAGS) -DFRONTEND -c $< -o $@

$(top_builddir)/pglite/src/pglitec/pglitec.o: $(top_builddir)/pglite/src/pglitec/pglitec.c $(top_builddir)/pglite/src/pglitec/pglitec.h
	$(CC) $(PGLITEC_CFLAGS) $(CFLAGS_SL) $(CPPFLAGS) -c $< -o $@

pglite_initdb.o: $(top_srcdir)/src/bin/initdb/initdb.c $(top_builddir)/src/Makefile.global $(top_builddir)/pglite/src/pglitec/pglitec.h
	$(CC) $(PGLITE_BUILD_CFLAGS) $(INITDB_REMAP_CFLAGS) $(CFLAGS_SL) $(CPPFLAGS) $(INITDB_CPPFLAGS) -c $< -o $@

pglite_findtimezone.o: $(top_srcdir)/src/bin/initdb/findtimezone.c $(top_builddir)/src/Makefile.global
	$(CC) $(PGLITE_BUILD_CFLAGS) $(CFLAGS_SL) $(CPPFLAGS) $(INITDB_FINDTIMEZONE_CPPFLAGS) -c $< -o $@

all-lib: submake-libpq submake-libpgfeutils $(BACKEND_OBJFILES) $(top_builddir)/src/common/libpgcommon_srv.a $(top_builddir)/src/port/libpgport_srv.a

clean: clean-lib
	rm -f libpglite.o pglite_stringinfo.o pglite_initdb.o pglite_findtimezone.o
