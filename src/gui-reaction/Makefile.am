AM_CPPFLAGS  = @UAE_CPPFLAGS@
AM_CPPFLAGS += -I$(top_srcdir)/src/include -I$(top_builddir)/src -I$(top_srcdir)/src
AM_CFLAGS    = @UAE_CFLAGS@

noinst_LIBRARIES = libguidep.a

libguidep_a_SOURCES = gui_core.c gui_util.c gui_settings.c gui_pages.c page_media.c page_hardware.c page_display.c page_sound.c page_harddisk.c page_input.c page_shortcuts.c page_config.c locale_support.c locale_defs.c arexx.c
noinst_HEADERS      = reaction_gui.h locale_ids.h arexx.h
