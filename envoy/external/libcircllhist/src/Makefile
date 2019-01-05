.SUFFIXES: .lo .re .c

OS=$(shell uname)

Q=
ifeq ($(V),)
	Q=@
endif

LIBCIRCLLHIST_VERSION=0.0.1

prefix=/usr/local
exec_prefix=${prefix}
bindir=${exec_prefix}/bin
sbindir=${exec_prefix}/sbin
libdir=${exec_prefix}/lib
includedir=${prefix}/include
libexecdir=${exec_prefix}/libexec
localstatedir=${prefix}/var
datarootdir=${prefix}/share
datadir=${datarootdir}
mandir=${datarootdir}/man
mansubdir=@mansubdir@
docdir=${prefix}/${datarootdir}/doc/${PACKAGE}
sysconfdir=${prefix}/etc
srcdir=.
top_srcdir=..

CC=gcc
SHLD=gcc -shared
CPPFLAGS= -D_REENTRANT -I$(top_srcdir)/src
CFLAGS=-g -O2 -g -std=c99 -Wall -O5
SHCFLAGS=-fpic -g -O2 -g -std=c99 -O5
CLINKFLAGS=
LUACFLAGS=@LUACFLAGS@
PGCFLAGS=@PGCFLAGS@
LDFLAGS= -Wl,-E -L$(libdir)
RLDFLAG=-Wl,--rpath=
ifneq ($(strip $(RLDFLAG)),)
  LDFLAGS += -Wl,--rpath=$(libdir)
endif
SHLDFLAGS=
DEPFLAGS=-MM
MODULELD=gcc -shared
AR=/usr/bin/ar
RANLIB=ranlib
LIBS=-lm 
INSTALL=/usr/bin/install -c
MAPFLAGS=@MAPFLAGS@
CTFCONVERT=
CTFMERGE=

# Later versions of ctf* tools no longer have -g and by default do not strip,
# so we get the same behavior as with -g on older versions.
ifneq ($(wildcard $(CTFCONVERT)),)
  HAS_G=$(shell $(CTFCONVERT) 2>&1 | grep -- -gis)
  ifneq ($(HAS_G),)
    CTFNOSTRIP=-g
  else
    CTFNOSTRIP=
  endif
endif

WHOLE_ARCHIVE=-Wl,--whole-archive
NOWHOLE_ARCHIVE=-Wl,--no-whole-archive
LIBCIRCLLHIST_V=libcircllhist.so.$(LIBCIRCLLHIST_VERSION)
LIBCIRCLLHIST=libcircllhist.so
LIBCIRCLLHISTA=libcircllhist.a

ifeq ($(OS),Darwin)
SHLDFLAGS+=-current_version $(LIBCIRCLLHIST_VERSION) -install_name $(libdir)/$(LIBCIRCLLHIST_V)
endif

LUA_FFI=lua/ffi_libcircllhist.lua
PYTHON_FFI=python/circllhist/ffi.py

TARGETS=$(LIBCIRCLLHISTA) $(LIBCIRCLLHIST) $(LUA_FFI) $(PYTHON_FFI) test/histogram_test test/histogram_perf

all:	reversion $(TARGETS)

HEADERS=circllhist.h

LIBCIRCLLHIST_OBJS=circllhist.lo

.PHONY:	reversion

reversion:
	@$(top_srcdir)/buildtools/mkversion.sh circllhist_version.h

circllhist_version.h:
	@$(top_srcdir)/buildtools/mkversion.sh circllhist_version.h

$(LIBCIRCLLHISTA):	$(LIBCIRCLLHIST_OBJS)
	@rm -f $(LIBCIRCLLHISTA)
	$(Q)$(AR) cq $(LIBCIRCLLHISTA) $(LIBCIRCLLHIST_OBJS)

$(LIBCIRCLLHIST):	$(LIBCIRCLLHIST_V)
	ln -sf $(LIBCIRCLLHIST_V) $(LIBCIRCLLHIST)

$(LIBCIRCLLHIST_V):	$(LIBCIRCLLHIST_OBJS)
	@echo "- linking $@"
	$(SHLD) $(SHLDFLAGS) $(CFLAGS) -o $@ $(LIBCIRCLLHIST_OBJS) -lm
	$(Q)if test -x "$(CTFMERGE)" ; then \
		echo "- merging CTF ($@)" ; \
		 $(CTFMERGE) -l @LIBCIRCLLHIST_VERSION@ -o $@ $(LIBCIRCLLHIST_OBJS) ; \
	fi

test/histogram_test: test/histogram_test.c $(LIBCIRCLLHIST)
	$(Q)$(CC) -I. $(CPPFLAGS) $(CFLAGS) -L. $(LDFLAGS) -I. -o $@ test/histogram_test.c -lcircllhist

test/histogram_perf: test/histogram_perf.c $(LIBCIRCLLHIST)
	$(Q)$(CC) -I. $(CPPFLAGS) $(CFLAGS) -L. $(LDFLAGS) -I. -o $@ test/histogram_perf.c -lcircllhist -lm

circllhist.ffi.h: circllhist.h
	./prepareFFI.sh < $< > $@

$(LUA_FFI): circllhist.ffi.h
	mkdir -p $$(dirname $@)
	./generateLuaFiles.sh < $< > $@

$(PYTHON_FFI): circllhist.ffi.h
	mkdir -p $$(dirname $@)
	./generatePythonFiles.sh < $< > $@

.c.lo:
	@echo "- compiling $<"
	$(Q)$(CC) $(CPPFLAGS) $(SHCFLAGS) -c $< -o $@
	$(Q)cp $@ $@.noctf
	$(Q)if test -x "$(CTFCONVERT)" ; then \
		echo "- making CTF ($@)" ; \
		$(CTFCONVERT) $(CTFNOSTRIP) -i -l @VERSION@ $@.noctf ; \
	fi
	$(Q)mv $@.noctf $@

.c.o:
	@echo "- compiling $<"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c $<

install-headers:	$(HEADERS)
	$(top_srcdir)/buildtools/mkinstalldirs $(DESTDIR)$(includedir)
	for file in $(HEADERS) ; do \
		$(INSTALL) -m 0644 $$file $(DESTDIR)$(includedir)/$$file ; \
	done

install-libs:    $(LIBCIRCLLHIST) $(LUA_FFI)
	$(top_srcdir)/buildtools/mkinstalldirs $(DESTDIR)$(libdir)
	$(top_srcdir)/buildtools/mkinstalldirs $(DESTDIR)$(datadir)/lua/5.1
	$(INSTALL) -m 0755 $(LIBCIRCLLHIST_V) $(DESTDIR)$(libdir)/$(LIBCIRCLLHIST_V)
	$(INSTALL) -m 0755 $(LIBCIRCLLHISTA) $(DESTDIR)$(libdir)/$(LIBCIRCLLHISTA)
	ln -sf $(LIBCIRCLLHIST_V) $(DESTDIR)$(libdir)/$(LIBCIRCLLHIST)
	$(INSTALL) -m 0755 $(LUA_FFI) $(DESTDIR)$(datadir)/lua/5.1/

install:	install-headers install-libs

install-python:
	cd python && python setup.py install

tests: test/histogram_test test/histogram_perf $(LUA_FFI) $(PYTHON_FFI)
	test/runTest.sh

tests-python:
	python python/test.py

clean:
	rm -f *.lo *.o $(TARGETS)
	rm -f $(LIBCIRCLLHIST) $(LIBCIRCLLHIST_V)
	rm -f histogram_test
	rm -f histogram_perl
	rm -f circllhist.ffi.h
	rm -f $(LUA_FFI)
	rm -f $(PYTHON_FFI)

Makefile.dep:
	$(CC) -I. $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) *.c > Makefile.dep

include Makefile.dep

distclean:	clean
