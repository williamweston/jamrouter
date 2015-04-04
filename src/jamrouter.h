/*****************************************************************************
 *
 * jamrouter.h
 *
 * JAMRouter:  JACK <--> ALSA MIDI Router
 *
 * Copyright (C) 1999-2015 William Weston <william.h.weston@gmail.com>
 *
 * JAMROUTER is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * JAMROUTER is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with JAMROUTER.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************/
#ifndef _JAMROUTER_H_
#define _JAMROUTER_H_

#include <pthread.h>
#include <sys/param.h>
#include "../config.h"
#include "driver.h"


/*****************************************************************************
 *
 * Tunable Compile Time Constants
 *
 *****************************************************************************/


/* Enable debug output (test thoroughly if disabling!!!).  For now,
   all error / warn / debug output is handled by debug thread until
   all other threads have started.  Debug output is then handled by
   the watchdog loop. */
#define ENABLE_DEBUG

/* Default realtime thread priorities. */
/* These can be changed at runtime in the preferences. */
#define MIDI_RX_THREAD_PRIORITY         68
#define MIDI_TX_THREAD_PRIORITY         68
#define JACK_THREAD_PRIORITY            66

/* Default realtime scheduling policy for midi and midi_tx threads. */
#define JAMROUTER_SCHED_POLICY          SCHED_FIFO

/* MIDI options */
#define DEFAULT_MIDI_DRIVER             MIDI_DRIVER_RAW_GENERIC

/* JACK Options */
#define ENABLE_JACK_LATENCY_CALLBACK

/* Define ALSA_SCAN_SUBDEVICES to scan individual audio / MIDI
   subdevices.  In practice, almost everyone ignores the subdevices,
   and this should not be necessary. */
//define ALSA_SCAN_SUBDEVICES

/* Phase of MIDI period for synchronizing JACK buffer processing period starts.
   This can now be set on the command line, so there should be no reason to
   change from the default here. */
#define DEFAULT_MIDI_PHASE_LOCK        0.5

/* max number of samples to use in the ringbuffer. */
/* must be a power of 2, and must handle at least */
/* DEFAULT_BUFFER_PERIODS * 2048. */
#define MAX_BUFFER_SIZE                 8192   /* 4 periods of 2048 */
#define MAX_BUFFER_PERIODS              16
#define DEFAULT_BUFFER_PERIODS          4
#define DEFAULT_BUFFER_PERIOD_SIZE      256
#define DEFAULT_LATENCY_PERIODS         1
#define DEFAULT_SAMPLE_RATE             48000

/* For now, JAMRouter has 2 single-reader-single-writer event queues */
#define MAX_MIDI_QUEUES                 2
#define A2J_QUEUE                       0x0
#define J2A_QUEUE                       0x1

/* Raw MIDI options */

/* Generic Raw MIDI and ALSA Raw MIDI are stable.
   These should be enabled for most builds. */
#define ENABLE_RAWMIDI_ALSA_RAW
#define ENABLE_RAWMIDI_GENERIC

/* OSS /dev/sequencer has only been tested with ALSA OSS emulation. */
//define ENABLE_RAWMIDI_OSS

/* OSS /dev/sequencer2 support is new and only support Rx w/o sysex. */
//define ENABLE_RAWMIDI_OSS2

/* multibyte may be unreliable on some hardware.  Byte at a time works well. */
//define RAWMIDI_ALSA_MULTI_BYTE_IO

/* Duplex operation works.  MPU-401 duplex issues are possibly a driver
   problem with some MPU-401 variants.  No known JAMRouter specific
   full duplex MIDI issues exist. */
#define RAWMIDI_ALSA_DUPLEX

/* ALSA RawMIDI options: */
/* ALSA RawMIDI works well with nonblocking IO on Rx and Tx. */
/* ALSA RawMIDI always uses poll() for nonblocking IO */
#define RAWMIDI_ALSA_NONBLOCK
#define RAWMIDI_ALSA_NONBLOCK_TX

/* nonblocking with poll() seems to perform the best for generic raw. */
#define RAWMIDI_GENERIC_NONBLOCK

/* poll() on raw MIDI is working properly for all tested devices.
   Jitter performance is reliable without the use of poll(), however
   Rx CPU usage goes up considerably when disabled.  Leave this
   option enabled unless poll() is broken for your device / driver. */
#define RAWMIDI_USE_POLL

/* poll() on OSS is currently working with the current OSS code. */
#define RAWMIDI_OSS_USE_POLL

/* flush code hasn't been tested in years, and may no longer be necessary. */
//define RAWMIDI_FLUSH_ON_START
//define RAWMIDI_FLUSH_NONBLOCK

/* Default raw MIDI device to use when none is specified. */
#define RAWMIDI_ALSA_DEVICE             "hw:0,0"
#define RAWMIDI_RAW_DEVICE              "/dev/midi"
#define RAWMIDI_OSS_DEVICE              "/dev/sequencer"
#define RAWMIDI_OSS2_DEVICE             "/dev/sequencer2"


/*****************************************************************************
 *
 * Leave disabled unless actively working on affected code:
 *
 *****************************************************************************/



/*****************************************************************************
 *
 * Type definitions and Preprocessor macros used throughout the codebase
 *
 *****************************************************************************/


/* Type to use for (almost) all floating point math. */
# if (ARCH_BITS == 64)
#  define MATH_64_BIT
typedef double sample_t;
# else
#  define MATH_32_BIT
typedef float sample_t;
# endif


/* Insist that funtion arguments are USED or UNUSED, if necessary */
#if defined(USED)
#elif defined(__GNUC__)
# define USED(x) x
#else
# define USED(x) x
#endif

#if defined(UNUSED)
#elif defined(__GNUC__)
# define UNUSED(x) x __attribute__ ((unused))
#else
# define UNUSED(x) x
#endif

#if !defined(ENABLE_INPUTS) && defined(__GNUC__)
# define USED_FOR_INPUTS(x) x __attribute__ ((unused))
#else
# define USED_FOR_INPUTS(x) x
#endif

/* Number of elements in an array */
#define NELEM(a)            ( sizeof(a) / sizeof((a)[0]) )

/* 32-/64-bit math */
#ifdef MATH_32_BIT
# ifdef MATH_64_BIT
#  undef MATH_64_BIT
# endif
# define MATH_SIN(x) sinf(x)
# define MATH_COS(x) cosf(x)
# define MATH_ABS(x) fabsf(x)
# define MATH_EXP(x) expf(x)
# define MATH_LOG(x) logf(x)
# define MATH_SQRT(x) sqrtf(x)
# define MATH_FLOOR(x) floorf(x)
# define MATH_ATAN2(x) atan2f(x)
#endif
#ifdef MATH_64_BIT
# ifdef MATH_32_BIT
#  undef MATH_32_BIT
# endif
# define MATH_SIN(x) sin(x)
# define MATH_COS(x) cos(x)
# define MATH_ABS(x) fabs(x)
# define MATH_EXP(x) exp(x)
# define MATH_LOG(x) log(x)
# define MATH_SQRT(x) sqrt(x)
# define MATH_FLOOR(x) floor(x)
# define MATH_ATAN2(x) atan2(x)
#endif


/*****************************************************************************
 *
 * Globals and prototypes from jamrouter.c
 *
 *****************************************************************************/

extern char            jamrouter_cmdline[512];
extern char            jamrouter_full_cmdline[512];

extern pthread_t       debug_thread_p;
extern pthread_t       midi_rx_thread_p;
extern pthread_t       midi_tx_thread_p;
extern pthread_t       jack_thread_p;

extern char            *midi_rx_port_name;
extern char            *midi_tx_port_name;
extern char            *jack_input_port_name;
extern char            *jack_output_port_name;

extern int             midi_rx_thread_priority;
extern int             midi_tx_thread_priority;

extern int             lash_disabled;
extern int             sample_rate;
extern int             pending_shutdown;
extern int             jamrouter_instance;
extern int             tx_prefer_all_notes_off;
extern int             tx_prefer_real_note_off;
extern int             rx_queue_real_note_off;
extern int             echotrans;
extern int             use_running_status;
extern int             active_sensing_mode;
extern int             byte_guard_time_usec;
extern int             event_guard_time_usec;
extern int             rx_latency_periods;
extern int             tx_latency_periods;
extern int             jitter_correct_mode;
#ifdef HAVE_JACK_GET_CYCLE_TIMES
extern int             jack_dll_level;
#endif
#ifndef WITHOUT_JUNO
extern int             translate_juno_sysex;
extern int             echosysex;
#endif

extern unsigned char   sysex_terminator;
extern unsigned char   sysex_extra_terminator;
extern unsigned char   note_on_velocity;
extern unsigned char   note_off_velocity;

extern unsigned char   keymap_tx_channel[16];
extern unsigned char   keymap_tx_controller[16];

extern unsigned char   pitchmap_tx_channel[16];
extern unsigned char   pitchmap_center_note[16];
extern unsigned char   pitchmap_bend_range[16];

extern unsigned char   pitchcontrol_tx_channel[16];
extern unsigned char   pitchcontrol_controller[16];


int get_instance_num(void);
void jamrouter_shutdown(const char *msg);
void init_rt_mutex(pthread_mutex_t *mutex, int rt);


#endif /* _JAMROUTER_H_ */
