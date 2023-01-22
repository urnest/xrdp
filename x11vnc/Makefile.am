AM_CPPFLAGS = \
  -DXRDP_CFG_PATH=\"${sysconfdir}/xrdp\" \
  -DXRDP_SBIN_PATH=\"${sbindir}\" \
  -DXRDP_SHARE_PATH=\"${datadir}/xrdp\" \
  -DXRDP_PID_PATH=\"${localstatedir}/run\" \
  -I$(top_srcdir)/common

module_LTLIBRARIES = \
  libx11vnc.la

libx11vnc_la_SOURCES = \
  x11vnc.c

libx11vnc_la_LIBADD = \
  $(top_builddir)/common/libcommon.la

if !MACOS
libx11vnc_la_LDFLAGS = -avoid-version -module
endif
