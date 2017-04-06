DRIVER = ocilib

pkglib_LTLIBRARIES = ocilib.la
ocilib_la_SOURCES = ocilib.cpp
ocilib_la_CPPFLAGS=-I@top_srcdir@/include -I@top_srcdir@/src/server/include @OCILIB_CPPFLAGS@
ocilib_la_LDFLAGS = -module -avoid-version @OCILIB_LDFLAGS@
ocilib_la_LIBADD = ../../../libnetxms/libnetxms.la @OCILIB_LIBS@

EXTRA_DIST = \
	ocilib.vcproj \
	ocilibdrv.h

install-exec-hook:
	if test "x`uname -s`" = "xAIX" ; then OBJECT_MODE=@OBJECT_MODE@ $(AR) x $(DESTDIR)$(pkglibdir)/$(DRIVER).a $(DESTDIR)$(pkglibdir)/$(DRIVER).so ; rm -f $(DESTDIR)$(pkglibdir)/$(DRIVER).a ; fi
	mkdir -p $(DESTDIR)$(pkglibdir)/dbdrv
	mv -f $(DESTDIR)$(pkglibdir)/$(DRIVER).so $(DESTDIR)$(pkglibdir)/dbdrv/$(DRIVER).ddr
	rm -f $(DESTDIR)$(pkglibdir)/$(DRIVER).la	
	rm -f $(DESTDIR)$(libdir)/libnxddr_$(DRIVER).so*
	ln -s netxms/dbdrv/$(DRIVER).ddr $(DESTDIR)$(libdir)/libnxddr_$(DRIVER).so
