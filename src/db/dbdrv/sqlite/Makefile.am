DRIVER = sqlite

pkglib_LTLIBRARIES = sqlite.la
sqlite_la_SOURCES = sqlite.cpp
sqlite_la_CPPFLAGS=-I@top_srcdir@/include -I@top_srcdir@/src/server/include
sqlite_la_LDFLAGS = -module -avoid-version
sqlite_la_LIBADD = ../../../libnetxms/libnetxms.la

if USE_INTERNAL_SQLITE
sqlite_la_CPPFLAGS += -I@top_srcdir@/src/sqlite
sqlite_la_LIBADD += ../../../sqlite/libnxsqlite.la
else
sqlite_la_LIBADD += -lsqlite3
endif

EXTRA_DIST = \
	Makefile.w32 \
	sqlite.vcproj \
	sqlitedrv.h

install-exec-hook:
	if test "x`uname -s`" = "xAIX" ; then OBJECT_MODE=@OBJECT_MODE@ $(AR) x $(DESTDIR)$(pkglibdir)/$(DRIVER).a $(DESTDIR)$(pkglibdir)/$(DRIVER).so ; rm -f $(DESTDIR)$(pkglibdir)/$(DRIVER).a ; fi
	mkdir -p $(DESTDIR)$(pkglibdir)/dbdrv
	mv -f $(DESTDIR)$(pkglibdir)/$(DRIVER).so $(DESTDIR)$(pkglibdir)/dbdrv/$(DRIVER).ddr
	rm -f $(DESTDIR)$(pkglibdir)/$(DRIVER).la	
	rm -f $(DESTDIR)$(libdir)/libnxddr_$(DRIVER).so*
	ln -s netxms/dbdrv/$(DRIVER).ddr $(DESTDIR)$(libdir)/libnxddr_$(DRIVER).so
