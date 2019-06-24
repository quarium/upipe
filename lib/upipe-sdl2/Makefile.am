lib_LTLIBRARIES = libupipe_sdl2.la

libupipe_sdl2_la_SOURCES = \
    upipe_sdl2_sink.c
libupipe_sdl2_la_CPPFLAGS = -I$(top_builddir)/include -I$(top_srcdir)/include
libupipe_sdl2_la_CFLAGS = $(AM_CFLAGS) $(GLESV2_CFLAGS)
libupipe_sdl2_la_LIBADD = $(SDL2_LIBS) $(GLESV2_LIBS)

libupipe_sdl2_la_LDFLAGS = -no-undefined -lSDL2

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libupipe_sdl2.pc
