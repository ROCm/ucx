#
# Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2020. ALL RIGHTS RESERVED.
#
# See file LICENSE for terms.
#

if HAVE_FUSE3

module_LTLIBRARIES      = libucs_fuse.la
libucs_fuse_la_CPPFLAGS = $(BASE_CPPFLAGS) $(FUSE3_CPPFLAGS)
libucs_fuse_la_CFLAGS   = $(BASE_CFLAGS) $(LT_CFLAGS)
libucs_fuse_la_LIBADD   = $(FUSE3_LIBS) \
                          $(top_builddir)/src/ucs/vfs/sock/libucs_vfs_sock.la \
                          $(top_builddir)/src/ucs/libucs.la
libucs_fuse_la_LDFLAGS  = $(FUSE3_LDFLAGS) -version-info $(SOVERSION)
libucs_fuse_la_SOURCES  = vfs_fuse.c

PKG_CONFIG_NAME=fuse

include $(top_srcdir)/config/module.am
include $(top_srcdir)/config/module-pkg-config.am

endif
