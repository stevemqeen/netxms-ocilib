DRIVER = mymobile

pkglib_LTLIBRARIES = mymobile.la
mymobile_la_SOURCES = main.cpp
mymobile_la_CPPFLAGS=-I@top_srcdir@/include
mymobile_la_LDFLAGS = -module -avoid-version
mymobile_la_LIBADD = ../../libnetxms/libnetxms.la -lcurl
if USE_INTERNAL_JANSSON
mymobile_la_LIBADD += @top_srcdir@/src/jansson/libnxjansson.la
else
mymobile_la_LIBADD += -ljansson
endif

EXTRA_DIST = \
	mymobile.vcproj

install-exec-hook:
	if test "x`uname -s`" = "xAIX" ; then OBJECT_MODE=@OBJECT_MODE@ $(AR) x $(DESTDIR)$(pkglibdir)/$(DRIVER).a $(DESTDIR)$(pkglibdir)/$(DRIVER).so ; rm -f $(DESTDIR)$(pkglibdir)/$(DRIVER).a ; fi
	mkdir -p $(DESTDIR)$(pkglibdir)/smsdrv
	mv -f $(DESTDIR)$(pkglibdir)/$(DRIVER).so $(DESTDIR)$(pkglibdir)/smsdrv/$(DRIVER).sms
	rm -f $(DESTDIR)$(pkglibdir)/$(DRIVER).la       
	rm -f $(DESTDIR)$(libdir)/libnxsms_$(DRIVER).so*
	ln -s netxms/smsdrv/$(DRIVER).sms $(DESTDIR)$(libdir)/libnxsms_$(DRIVER).so
