## Process this file with automake to produce Makefile.in

bin_PROGRAMS    = jamrouter

jamrouter_SOURCES  = \
	alsa_seq.c alsa_seq.h \
	debug.c debug.h \
	driver.c driver.h \
	jack.c jack.h \
	jack_midi.c jack_midi.h \
	jamrouter.c jamrouter.h \
	mididefs.h \
	midi_event.c midi_event.h \
	rawmidi.c rawmidi.h \
	timekeeping.c timekeeping.h

if WITH_LASH
    jamrouter_SOURCES  += lash.c lash.h
endif

if WITH_JUNO
    jamrouter_SOURCES  += juno.c juno.h
endif


AM_CFLAGS       = @JAMROUTER_CFLAGS@
AM_CPPFLAGS     = $(EXTRA_CPPFLAGS) @JAMROUTER_CPPFLAGS@
jamrouter_LDADD = $(INTLLIBS) @JAMROUTER_LIBS@


clean-local:


distclean-local:
	rm -f *~
