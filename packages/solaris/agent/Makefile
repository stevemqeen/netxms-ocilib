NX_VERSION=$(shell grep AC_INIT ../../../configure.ac | cut -d[ -f3| cut -d] -f1)
PLATFORM=$(shell uname -p)

ifdef SUFIX
	_SUFFIX=$(SUFFIX).
else
	_SUFFIX=
endif

all: package

package:
	mkdir -p /opt/netxms/svc
	cp files/nxagentd.xml /var/svc/manifest/application/management/
	cp files/nxagentd /opt/netxms/svc/
	sed 's,@version@,$(NX_VERSION),' < pkginfo.in > pkginfo
	rm -rf /tmp/RADnxagent
	pkgmk -o -r / -d /tmp -f Prototype
	pkgtrans /tmp /tmp/RADnxagent.$(NX_VERSION).$(_SUFFIX)$(PLATFORM).pkg RADnxagent
	gzip -f9 /tmp/RADnxagent.$(NX_VERSION).$(_SUFFIX)$(PLATFORM).pkg
	rm -rf /tmp/RADnxagent
	rm pkginfo
	rm -f /var/svc/manifest/application/management/nxagentd.xml
	rm -f /opt/netxms/svc/nxagentd
	rmdir /opt/netxms/svc
