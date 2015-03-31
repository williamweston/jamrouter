/*****************************************************************************
 *
 * timekeeping.h
 *
 * JAMRouter:  JACK <--> ALSA MIDI Router
 *
 * Copyright (C) 2012-2015 William Weston <william.h.weston@gmail.com>
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
 *
 *****************************************************************************/
#ifndef _TIMEKEEPING_H_
#define _TIMEKEEPING_H_

#include <time.h>
#include <glib.h>
#ifdef HAVE_JACK_GET_CYCLE_TIMES
#include <jack.h>
#endif
#include "jamrouter.h"


#define JAMROUTER_CLOCK_INIT           1111111111

#define FRAME_LIMIT_LOWER              0x1
#define FRAME_FIX_LOWER                0x2
#define FRAME_LIMIT_UPPER              0x4
#define FRAME_TIMESTAMP                0x8

#define TIME_NE                        0x0
#define TIME_EQ                        0x1
#define TIME_GT                        0x2
#define TIME_GE                        0x3
#define TIME_LT                        0x4
#define TIME_LE                        0x5


#if (ARCH_BITS == 32)
typedef float timecalc_t;
#endif /* (ARCH_BITS == 32) */
#if (ARCH_BITS == 64)
typedef double timecalc_t;
#endif /* (ARCH_BITS == 64) */

#if 0
struct int_timestamp {
	int              tv_sec;
	int              tv_nsec;
} __attribute__((packed));
typedef struct int_timestamp INT_TIMESTAMP;


union timestamp {
	struct timespec  ts;
	INT_TIMESTAMP    int;
} __attribute__((__transparent_union__));

typedef union timestamp TIMESTAMP;
#endif

typedef struct timespec TIMESTAMP;


typedef struct sync_info {
	unsigned short   rx_index;
	unsigned short   tx_index;
	unsigned short   input_index;
	unsigned short   output_index;
	unsigned short   buffer_size;
	unsigned short   buffer_size_mask;
	unsigned short   buffer_periods;
	unsigned short   buffer_period_size;
	unsigned short   period_mask;
	unsigned short   rx_latency_periods;
	unsigned short   tx_latency_periods;
	unsigned short   rx_latency_size;
	unsigned short   tx_latency_size;
	signed short     jack_wakeup_frame;
	timecalc_t       nsec_per_frame;
	timecalc_t       nsec_per_period;
	timecalc_t       f_buffer_period_size;
	timecalc_t       f_sample_rate;
	unsigned int     sample_rate;
	TIMESTAMP        start_time;
	TIMESTAMP        end_time;
	TIMESTAMP        sensing_timeout[MAX_MIDI_QUEUES];
#ifdef HAVE_JACK_GET_CYCLE_TIMES
	jack_nframes_t   jack_frames;
#endif
} SYNC_INFO;


extern volatile SYNC_INFO   sync_info[MAX_BUFFER_PERIODS];

extern clockid_t            system_clockid;

extern timecalc_t           midi_phase_lock;
extern timecalc_t           midi_phase_min;
extern timecalc_t           midi_phase_max;
extern timecalc_t           setting_midi_phase_lock;

extern int                  max_event_latency;


int timecmp(volatile TIMESTAMP *a,
            volatile TIMESTAMP *b,
            unsigned char mode);
void time_add(volatile TIMESTAMP *a,
              volatile TIMESTAMP *b);
void time_sub(volatile TIMESTAMP *a,
              volatile TIMESTAMP *b);
void time_add_nsecs(volatile TIMESTAMP *a,
                    int nsecs);
void time_sub_nsecs(volatile TIMESTAMP *a,
                    int nsecs);
timecalc_t time_nsecs(volatile TIMESTAMP *a);

void jamrouter_usleep(int usecs);
void jamrouter_nanosleep(long int nsecs);

unsigned short sleep_until_next_period(unsigned short period,
                                       TIMESTAMP *now);
void sleep_until_frame(unsigned short period,
                       unsigned short frame);

void set_midi_phase_lock(unsigned short period);
void start_midi_clock(void);

unsigned short get_midi_period(TIMESTAMP *now);
unsigned short get_midi_frame(unsigned short *period,
                              TIMESTAMP *now,
                              unsigned char flags);
TIMESTAMP *get_frame_time(unsigned short period,
                          unsigned short frame,
                          TIMESTAMP *frame_time);

void set_new_period_size(unsigned short period,
                         unsigned short nframes);
void init_sync_info(unsigned int sample_rate,
                    unsigned short period_size);

unsigned short set_midi_cycle_time(unsigned short period,
                                   int nframes);

void set_active_sensing_timeout(unsigned short period,
                                unsigned char queue_num);
int check_active_sensing_timeout(unsigned short period,
                                 unsigned char queue_num);


#endif /* _TIMEKEEPING_H_ */
