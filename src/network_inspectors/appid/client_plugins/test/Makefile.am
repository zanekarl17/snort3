
AM_DEFAULT_SOURCE_EXT = .cc

check_PROGRAMS = \
client_app_smtp_test

TESTS = $(check_PROGRAMS)

client_app_smtp_test_CPPFLAGS = -I$(top_srcdir)/src/network_inspectors/appid @AM_CPPFLAGS@ @CPPUTEST_CPPFLAGS@

client_app_smtp_test_LDADD = \
../../appid_stats_counter.o \
../../../../utils/libutils.a \
@CPPUTEST_LDFLAGS@

