bin_PROGRAMS = nxadm
nxadm_SOURCES = comm.cpp nxadm.cpp
nxadm_CPPFLAGS=-I@top_srcdir@/include -I@top_srcdir@/src/server/include
nxadm_LDADD = ../../../libnetxms/libnetxms.la ../../libnxsrv/libnxsrv.la ../../../snmp/libnxsnmp/libnxsnmp.la @SERVER_LIBS@

EXTRA_DIST = \
	nxadm.vcproj \
	nxadm.h
