/*****************************************************************************
 *
 * timekeeping.c
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
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <pthread.h>
#include <time.h>
#ifndef HAVE_CLOCK_GETTIME
#include <sys/time.h>
#endif
#include <asoundlib.h>
#include <glib.h>
#include "jamrouter.h"
#include "timekeeping.h"
#include "mididefs.h"
#include "midi_event.h"
#include "debug.h"
#include "driver.h"


volatile SYNC_INFO      sync_info[MAX_BUFFER_PERIODS];

clockid_t               system_clockid        = CLOCK_MONOTONIC;

struct timespec         jack_start_time       = { 0, JAMROUTER_CLOCK_INIT };

timecalc_t              midi_phase_lock       = 0;
timecalc_t              midi_phase_min        = 1.0;
timecalc_t              midi_phase_max        = 127.0;
timecalc_t              setting_midi_phase_lock = DEFAULT_MIDI_PHASE_LOCK;

int                     max_event_latency     = 0;


/*****************************************************************************
 * timecmp()
 *****************************************************************************/
int
timecmp(volatile TIMESTAMP *a, volatile TIMESTAMP *b, unsigned char mode) {
	switch (mode) {
	case TIME_NE:
		return ((a->tv_sec != b->tv_sec) || (a->tv_nsec != b->tv_nsec));
		break;
	case TIME_EQ:
		return ((a->tv_sec == b->tv_sec) && (a->tv_nsec == b->tv_nsec));
		break;
	case TIME_GT:
		return ((a->tv_sec > b->tv_sec) ||
		        ((a->tv_sec == b->tv_sec) && (a->tv_nsec > b->tv_nsec)));
		break;
	case TIME_GE:
		return ((a->tv_sec > b->tv_sec) ||
		        ((a->tv_sec == b->tv_sec) && (a->tv_nsec >= b->tv_nsec)));
		break;
	case TIME_LT:
		return ((a->tv_sec < b->tv_sec) ||
		        ((a->tv_sec == b->tv_sec) && (a->tv_nsec < b->tv_nsec)));
		break;
	case TIME_LE:
		return ((a->tv_sec < b->tv_sec) ||
		        ((a->tv_sec == b->tv_sec) && (a->tv_nsec <= b->tv_nsec)));
		break;
	default:
		return 0;
		break;
	}
}


/*****************************************************************************
 * time_add()
 *****************************************************************************/
void
time_add(volatile TIMESTAMP *a, volatile TIMESTAMP *b) {
	a->tv_sec  += b->tv_sec;
	a->tv_nsec += b->tv_nsec;
	while (a->tv_nsec >= 1000000000) {
		a->tv_nsec -= 1000000000;
		a->tv_sec++;
	}
	while (a->tv_nsec < 0) {
		a->tv_nsec += 1000000000;
		a->tv_sec--;
	}
}


/*****************************************************************************
 * time_sub()
 *****************************************************************************/
void
time_sub(volatile TIMESTAMP *a, volatile TIMESTAMP *b) {
	a->tv_sec -= b->tv_sec;
	if (b->tv_nsec > a->tv_nsec) {
		a->tv_nsec = a->tv_nsec + 1000000000 - b->tv_nsec;
		a->tv_sec--;
	}
	else {
		a->tv_nsec -= b->tv_nsec;
	}
	while (a->tv_nsec >= 1000000000) {
		a->tv_nsec -= 1000000000;
		a->tv_sec++;
	}
	while (a->tv_nsec < 0) {
		a->tv_nsec += 1000000000;
		a->tv_sec--;
	}
}


/*****************************************************************************
 * time_add_nsecs()
 *****************************************************************************/
void
time_add_nsecs(volatile TIMESTAMP *a, int nsecs) {
	int nsec = nsecs;

	while (nsec > 1000000000) {
		nsec -= 1000000000;
		a->tv_sec++;
	}
	while (nsec < 0) {
		nsec += 1000000000;
		a->tv_sec--;
	}
	a->tv_nsec += nsec;
	while (a->tv_nsec >= 1000000000) {
		a->tv_nsec -= 1000000000;
		a->tv_sec++;
	}
	while (a->tv_nsec < 0) {
		a->tv_nsec += 1000000000;
		a->tv_sec--;
	}
}


/*****************************************************************************
 * time_sub_nsecs()
 *****************************************************************************/
void
time_sub_nsecs(volatile TIMESTAMP *a, int nsecs) {
	int nsec = nsecs;

	while (nsec > 1000000000) {
		nsec -= 1000000000;
		a->tv_sec--;
	}
	while (nsec < 0) {
		nsec += 1000000000;
		a->tv_sec++;
	}
	if (a->tv_nsec < nsec) {
		a->tv_nsec = a->tv_nsec + 1000000000 - nsec;
		a->tv_sec--;
	}
	else {
		a->tv_nsec -= nsec;
	}
	while (a->tv_nsec >= 1000000000) {
		a->tv_nsec -= 1000000000;
		a->tv_sec++;
	}
	while (a->tv_nsec < 0) {
		a->tv_nsec += 1000000000;
		a->tv_sec--;
	}
}


/*****************************************************************************
 * time_nsecs()
 *****************************************************************************/
timecalc_t
time_nsecs(volatile TIMESTAMP *a)
{
	return (((timecalc_t)(a->tv_sec) * (timecalc_t)(1000000000)) +
	        (timecalc_t)(a->tv_nsec));
}


/*****************************************************************************
 * clock_gettime()
 *
 * replacement for systems without it.
 *****************************************************************************/
#ifndef HAVE_CLOCK_GETTIME
int
clock_gettime(clockid_t UNUSED(clockid), struct timespec *ts)
{
	struct timeval  tv;

	if (gettimeofday(&tv, NULL) == 0) {
		ts->tv_sec  = tv.tv_sec;
		ts->tv_nsec = tv.tv_usec * 1000;
		return 0;
	}
	return -1;
}
#endif /* !HAVE_CLOCK_GETTIME */


/*****************************************************************************
 * jamrouter_usleep()
 *****************************************************************************/
void
jamrouter_usleep(int usecs)
{
#ifdef HAVE_CLOCK_NANOSLEEP
	struct timespec         sleep_time       = { 0, usecs * 1000 };
#endif

	if (usecs > 0) {
#ifdef HAVE_CLOCK_NANOSLEEP
		clock_nanosleep(CLOCK_MONOTONIC, 0, &sleep_time, NULL);
#else
		usleep(sleep_time);
#endif
	}
}


/*****************************************************************************
 * jamrouter_nanosleep()
 *****************************************************************************/
void
jamrouter_nanosleep(long int nsecs)
{
#ifdef HAVE_CLOCK_NANOSLEEP
	struct timespec         sleep_time       = { 0, nsecs };
#endif

	if (nsecs > 5000) {
#ifdef HAVE_CLOCK_NANOSLEEP
		clock_nanosleep(CLOCK_MONOTONIC, 0, &sleep_time, NULL);
#else
		nanosleep(sleep_time);
#endif
	}
}


/*****************************************************************************
 * sleep_until_next_period()
 *
 * TODO:  Consider the use of select() to sleep for a maximum time instead
 *        a minimum time as with usleep() and clock_nanosleep().
 *****************************************************************************/
unsigned short
sleep_until_next_period(unsigned short period, TIMESTAMP *now)
{
	TIMESTAMP sleep_time;

	if ( (clock_gettime(system_clockid, now) == 0) &&
	     timecmp(now, &(sync_info[period].end_time), TIME_LT) ) {
		sleep_time.tv_sec  = sync_info[period].end_time.tv_sec;
		sleep_time.tv_nsec = sync_info[period].end_time.tv_nsec;
		time_sub(&sleep_time, now);
#ifdef HAVE_CLOCK_NANOSLEEP
		clock_nanosleep(CLOCK_MONOTONIC, 0, &sleep_time, NULL);
#else
		nanosleep(sleep_time);
#endif
	}

	period++;
	period &= sync_info[period].period_mask;

	sync_info[period].jack_wakeup_frame = 0;

	JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
	                DEBUG_COLOR_GREEN "! " DEBUG_COLOR_DEFAULT);

	/* When debug is disabled, a full memory fence is still needed here,
	   as MIDI Tx is about to dequeue. */
	asm volatile (" mfence; # read/write fence" : : : "memory");

	return period;
}


/*****************************************************************************
 * sleep_until_frame()
 *
 * TODO:  Consider the use of select() to sleep for a maximum time instead
 *        a minimum time as with usleep() and clock_nanosleep().
 *****************************************************************************/
void
sleep_until_frame(unsigned short period, unsigned short frame)
{
	TIMESTAMP now;
	TIMESTAMP frame_time;

	frame_time.tv_sec  = sync_info[period].start_time.tv_sec;
	frame_time.tv_nsec = sync_info[period].start_time.tv_nsec;

	time_add_nsecs(&frame_time,
	               (int)(sync_info[period].nsec_per_frame *
	                     (timecalc_t)(frame)));

	if ( (clock_gettime(system_clockid, &now) == 0) &&
	     (timecmp(&now, &frame_time, TIME_LT) ) ) {
		time_sub(&frame_time, &now);
		clock_nanosleep(CLOCK_MONOTONIC, 0, &frame_time, NULL);
	}
}


/*****************************************************************************
 * set_midi_phase_lock
 *****************************************************************************/
void
set_midi_phase_lock(unsigned short period)
{
	midi_phase_lock = (timecalc_t)(setting_midi_phase_lock *
	                               sync_info[period].f_buffer_period_size);

	/* If not changed from the default, set phase lock to 0.75 at 1-period rx
	   latency for 48000/64, 96000/128, 192000/256, etc. rather than enforcing
	   a minimum of 2 rx latency periods for these settings. */
	if ( ((sync_info[period].sample_rate /
	       sync_info[period].buffer_period_size) == 750) &&
	     (sync_info[period].rx_latency_periods == 1) &&
	     (setting_midi_phase_lock == DEFAULT_MIDI_PHASE_LOCK) ) {
		midi_phase_lock =
			(timecalc_t)(0.75) * sync_info[period].f_buffer_period_size;
	}

	/* When buffer size is 16, an extra Tx latency period is enforced,
	   allowing phase_min to be safely moved closer to 0.  This avoids
	   hard-latching the clock's phase (and introducing jitter) when jack
	   wakes up late. */
	if (sync_info[period].buffer_period_size == 16) {
			midi_phase_lock  = (timecalc_t)(5.0);
			midi_phase_min   = (timecalc_t)(2.0);
			midi_phase_max   = (timecalc_t)(8.0);
	}

	/* For all other buffer sizes, keep the phase lock within 8 samples of
	   either MIDI period boundary and set the jitter correction allowance to
	   +/- 3 frames. */
	else {
		if (midi_phase_lock < (timecalc_t)(8.0)) {
			midi_phase_lock  = (timecalc_t)(8.0);
		}
		if ( midi_phase_lock >
		     (sync_info[period].f_buffer_period_size - (timecalc_t)(9.0)) ) {
			midi_phase_lock =
				sync_info[period].f_buffer_period_size - (timecalc_t)(9.0);
		}

		midi_phase_min  = midi_phase_lock - (timecalc_t)(3.0);
		midi_phase_max  = midi_phase_lock + (timecalc_t)(3.0);
	}
}


/*****************************************************************************
 * start_midi_clock()
 *
 * Initializes MIDI timing variables based on variables set during audio
 * initilization (buffer size, sample rate, etc.).
 *****************************************************************************/
void
start_midi_clock(void)
{
	TIMESTAMP     now;
	unsigned char       period = 0;

	jack_start_time.tv_sec  = 0;
	jack_start_time.tv_nsec = JAMROUTER_CLOCK_INIT;

	sync_info[period].nsec_per_period =
		sync_info[period].f_buffer_period_size *
		(timecalc_t)(1000000000.0) /
		(timecalc_t)(sync_info[period].sample_rate);

	sync_info[period].nsec_per_frame =
		sync_info[period].nsec_per_period
		/ sync_info[period].f_buffer_period_size;

	set_midi_phase_lock(period);

	/* now initialize the reference timestamps. */
#ifdef CLOCK_MONOTONIC
	if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
		system_clockid = CLOCK_MONOTONIC;
	}
#endif
#ifdef CLOCK_MONITONIC_HR
	if (clock_gettime(CLOCK_MONOTONIC_HR, &now) == 0) {
		system_clockid = CLOCK_MONOTONIC_HR;
	}
#endif
#ifdef CLOCK_MONOTONIC_RAW
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &now) == 0) {
		system_clockid = CLOCK_MONOTONIC_RAW;
	}
#endif
	if (clock_gettime(system_clockid, &now) == 0) {
		for (period = 0; period < DEFAULT_BUFFER_PERIODS; period++) {
			sync_info[period].start_time.tv_sec  = now.tv_sec;
			sync_info[period].start_time.tv_nsec = now.tv_nsec;
			/* initialize the active sensing timeout to zero (off). */
			sync_info[period].sensing_timeout[A2J_QUEUE].tv_sec  = 0;
			sync_info[period].sensing_timeout[A2J_QUEUE].tv_nsec =
				JAMROUTER_CLOCK_INIT;
			sync_info[period].sensing_timeout[J2A_QUEUE].tv_sec  = 0;
			sync_info[period].sensing_timeout[J2A_QUEUE].tv_nsec =
				JAMROUTER_CLOCK_INIT;
		}
	}

}


/*****************************************************************************
 * get_midi_period()
 *****************************************************************************/
unsigned short
get_midi_period(TIMESTAMP *now)
{
	TIMESTAMP          recent           = { 0, 0 };
	TIMESTAMP          delta;
	timecalc_t         delta_nsec;
	unsigned short     last_period      = sync_info[0].period_mask;
	unsigned short     recent_period    = 0;
	unsigned short     period;
	unsigned short     elapsed_periods;
	
	if (clock_gettime(system_clockid, now) != 0) {
		jamrouter_shutdown("clock_gettime() failed!\n");
	}

	/* Any thread needing to know what period we are in right now should under
	   no circumstances use invalid sync_info[] for the current period.  This
	   memory fence assures us that the compiler and the CPU do not do the
	   wrong thing here.  In the case where current sync_info[] is not
	   fetched, a true CPU cache latency miss, extra logic is included below
	   so that JAMRouter does not do the wrong thing when it comes to
	   synchronization. */
	asm volatile ("mfence; # read/write fence" : : : "memory");

	for (period = 0; period < sync_info[0].buffer_periods; period++) {
		/* check timestamps to find current MIDI period */
		if ( ( timecmp(now, &(sync_info[last_period].end_time), TIME_GT) ||
		       timecmp(now, &(sync_info[period].start_time),    TIME_GE) ) &&
		     timecmp(now, &(sync_info[period].end_time),    TIME_LE) ) {
			return (unsigned short)(period);
		}
		/* keep track of most recent sync_info found. */
		if ( timecmp(&recent, &(sync_info[period].start_time),    TIME_LE) ||
		     timecmp(&recent, &(sync_info[last_period].end_time), TIME_LT) ) {
			recent.tv_sec  = sync_info[period].start_time.tv_sec;
			recent.tv_nsec = sync_info[period].start_time.tv_nsec;
			recent_period = period;
		}
		last_period = period;
	}

	/* Control reaches this point only before clock starts or when
       sync_info[period] is found to be stale.  This extra logic only becomes
       necessary when the number of periods in the sync_info[] ringbuffer is
       pushed lower than 4, yet it remains as an extra safeguard against
       against synchronization error, as the behavior of this logic has shown
       itself to be 100% correct.  In the case of any sort of synchronization
       error, timing information is dependably extrapolated from the most
       recent sync_info in the ringbuffer.  In the case of buffer size changes
       and synchronization failure at the same time, the MIDI threads will
       pick up the change exactly one period (pre jack_bufsize reckoning)
       late.  This should never be a concern during the middle of a
       performance. */
	delta.tv_sec  = now->tv_sec;
	delta.tv_nsec = now->tv_nsec;
	time_sub(&delta, &recent);
	delta_nsec = time_nsecs(&delta);
	elapsed_periods =
		(unsigned short)(delta_nsec /
		                 sync_info[recent_period].nsec_per_period);
	period =
		(unsigned short)((recent_period + elapsed_periods) &
		                 sync_info[recent_period].period_mask);

	JAMROUTER_DEBUG(DEBUG_CLASS_TESTING, DEBUG_COLOR_RED "}}%d{{ ", period);

	return period;
}


/*****************************************************************************
 * get_midi_frame()
 *
 * Returns the frame number corresponding to the supplied period and
 * timestamp.  Processing flags are:
 *
 * FRAME_LIMIT_LOWER:  Enforce zero as the lower frame limit.
 * FRAME_LIMIT_UPPER:  Enforce buffer period size minus one as upper limit.
 * FRAME_FIX_LOWER:    Negative values are indicative of the sync_info[]
 *   ringbuffer not being updated for the current thread, most likely due
 *   to old values being stuck in the CPU cache.  This option detects
 *   stale sync_info[] and adjusts the period and frame accordingly.
 *   At most buffer_size/sample_rate combinations, this is either not
 *   possible or an extremely rare corner case.  At 16/96000, this extra
 *   logic becomes an absolute necessity for rock-solid timing.  In all
 *   cases, this logic has been observed to make the proper correction
 *   100% of the time.
 *****************************************************************************/
unsigned short
get_midi_frame(unsigned short *period, TIMESTAMP *now, unsigned char flags)
{
	timecalc_t      period_time_nsec;
	short           frame;

	if (flags & FRAME_TIMESTAMP) {
		if (clock_gettime(system_clockid, now) != 0) {
			jamrouter_shutdown("clock_gettime() failed!\n");
		}
	}

	period_time_nsec =
		(timecalc_t)(((now->tv_sec - sync_info[*period].start_time.tv_sec) *
		              1000000000) +
		             (now->tv_nsec - sync_info[*period].start_time.tv_nsec));

	frame = (short)(period_time_nsec / sync_info[*period].nsec_per_frame);

	if (frame < 0) {
		if (flags & FRAME_LIMIT_LOWER) {
			frame = 0;
		}
		else if (flags & FRAME_FIX_LOWER) {
			while (frame < 0) {
				frame =	(short)(frame + (short)
					        (sync_info[*period].buffer_period_size));
				*period = ((unsigned short)
				           (*period + sync_info[*period].period_mask) &
				           (unsigned short)(sync_info[*period].period_mask));
			}
			JAMROUTER_DEBUG(DEBUG_CLASS_TESTING,
			                DEBUG_COLOR_RED "]]%d[[ ", *period);
		}
	}
	else if ( (flags & FRAME_LIMIT_UPPER) &&
	          (frame >= sync_info[*period].buffer_period_size) ) {
		frame = (short)(sync_info[*period].buffer_period_size);
		frame--;
	}

	return (unsigned short)(frame);
}


/*****************************************************************************
 * get_frame_time()
 *
 * Fills and returns the supplied timestamp with the time corresponding to
 * the supplied frame offset within the supplied period.
 *****************************************************************************/
TIMESTAMP *
get_frame_time(unsigned short   period,
               unsigned short   frame,
               TIMESTAMP        *frame_time)
{
	frame_time->tv_sec  = sync_info[period].start_time.tv_sec;
	frame_time->tv_nsec = sync_info[period].start_time.tv_nsec;

	time_add_nsecs(frame_time,
	               (int)(sync_info[period].nsec_per_frame * (double)(frame)));

	return frame_time;
}


/*****************************************************************************
 * get_delta_nsecs()
 *
 * Returns the floating point difference in nanoseconds between
 * two timestamps.
 *****************************************************************************/
timecalc_t
get_delta_nsecs(TIMESTAMP *now, volatile TIMESTAMP *start)
{
	if (clock_gettime(system_clockid, now) != 0) {
		jamrouter_shutdown("clock_gettime() failed!\n");
	}

	return (timecalc_t)((((timecalc_t)(now->tv_sec) -
	                      (timecalc_t)(start->tv_sec)) *
	                     (timecalc_t)(1000000000.0)) +
	                    ((timecalc_t)(now->tv_nsec) -
	                     (timecalc_t)(start->tv_nsec)));
}


/*****************************************************************************
 * set_new_period_size()
 *
 * Re-calculates all buffer period size dependent sync variables.
 * Sets the new buffer period size and related variables for the given period.
 * All buffer latencies are determined here and only here.
 *****************************************************************************/
void
set_new_period_size(unsigned short period, unsigned short nframes)
{
	unsigned short  max_latency     = 0;

	/* If command line values are not supplied, calculate default rx/tx
	   latency periods so that latency+jitter is never low enough to lead
	   to predictable realtime synchronization failure.  */
	if (rx_latency_periods > 0) {
		sync_info[period].rx_latency_periods =
			(unsigned short)(rx_latency_periods);
	}
	else {
		sync_info[period].rx_latency_periods =
			(unsigned short)((int)(sync_info[period].sample_rate) /
			                 (int)(nframes * 666));
	}
	if (tx_latency_periods > 0) {
		sync_info[period].tx_latency_periods =
			(unsigned short)(tx_latency_periods);
	}
	else {
		sync_info[period].tx_latency_periods =
			(unsigned short)((int)(sync_info[period].sample_rate) /
			                 (int)(nframes * 2160));
	}
	if (sync_info[period].rx_latency_periods == 0) {
		sync_info[period].rx_latency_periods++;
	}
	if (sync_info[period].tx_latency_periods == 0) {
		sync_info[period].tx_latency_periods++;
	}

	/* Determine number of buffer periods. */
	switch (nframes) {
	case 2048:
	case 1024:
	case 512:
	case 256:
		/* ignore extra command line latencies for large buffer sizes. */
		sync_info[period].rx_latency_periods = 1;
		sync_info[period].tx_latency_periods = 1;
		/* fall-through */
	case 128:
		/* When phase lock brings JACK and MIDI phases too close together
		   for -rt tolerances, increase rx or tx latency to compensate. */
		if ( ( ((timecalc_t)(nframes) * setting_midi_phase_lock) <
		       (sync_info[period].sample_rate / 1500) ) && 
		     (sync_info[period].rx_latency_periods < 2) ) {
			sync_info[period].rx_latency_periods = 2;
		}
		else if ( ( ((timecalc_t)(nframes) * setting_midi_phase_lock) >
		            (nframes - (sync_info[period].sample_rate / 1500)) ) &&
		          (sync_info[period].tx_latency_periods < 2) ) {
			sync_info[period].tx_latency_periods = 2;
		}
		sync_info[period].buffer_periods = 4;
		break;
	case 64:
	case 16:
	case 32:
		/* calculate power of 2 buffer_periods based on latencies */
		sync_info[period].buffer_periods = (unsigned short)(512 / nframes);
		if (sync_info[period].rx_latency_periods > max_latency) {
			max_latency = sync_info[period].rx_latency_periods;
		}
		if (sync_info[period].tx_latency_periods > max_latency) {
			max_latency = sync_info[period].tx_latency_periods;
		}
		if (sync_info[period].buffer_periods > (max_latency << 1)) {
			sync_info[period].buffer_periods >>= 1;
		}
		break;
	}

	/* set buffer size and corresponding mask values. */
	sync_info[period].period_mask          =
		(unsigned short)(sync_info[period].buffer_periods - 1);
	sync_info[period].buffer_period_size   = (unsigned short)(nframes);
	sync_info[period].f_buffer_period_size = (timecalc_t)(nframes);
	sync_info[period].buffer_size          =
		(unsigned short)(sync_info[period].buffer_period_size *
		                 sync_info[period].buffer_periods);
	sync_info[period].buffer_size_mask     =
		(unsigned short)(sync_info[period].buffer_size - 1);

	JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
	                "sync_info[%02d]:  Sample Rate: %d  "
	                "Periods: %d  Rx/Tx Latency Periods: %d/%d\n",
	                period, sync_info[period].sample_rate,
	                sync_info[period].buffer_periods,
	                sync_info[period].rx_latency_periods,
	                sync_info[period].tx_latency_periods);

	/* synchronized buffer indices based on period size and latencies. */
	sync_info[period].rx_latency_size =
		(unsigned short)(sync_info[period].rx_latency_periods * nframes);
	sync_info[period].tx_latency_size =
		(unsigned short)(sync_info[period].tx_latency_periods * nframes);

	sync_info[period].input_index  = (unsigned short)(period * nframes);
	sync_info[period].output_index = (unsigned short)(period * nframes);

	sync_info[period].rx_index =
		(unsigned short)((sync_info[period].output_index +
		                  sync_info[period].rx_latency_size) &
		                 sync_info[period].buffer_size_mask);

	sync_info[period].tx_index =
		(unsigned short)((sync_info[period].input_index +
		                  (sync_info[period].buffer_size -
		                   sync_info[period].tx_latency_size)) &
		                 sync_info[period].buffer_size_mask);

	/* nsec_per_period and nsec_per_frame depend on period size. */
	sync_info[period].nsec_per_period  =
		sync_info[period].f_buffer_period_size *
		(timecalc_t)(1000000000.0) /
		(timecalc_t)(sync_info[period].sample_rate);

	sync_info[period].nsec_per_frame =
		sync_info[period].nsec_per_period /
		sync_info[period].f_buffer_period_size;

	/* calculate new midi phase lock for current buffer size. */
	set_midi_phase_lock(period);
}


/*****************************************************************************
 * init_sync_info()
 *
 * Called once after audio system initialization to initialize the sync_info[]
 * ringbuffer once the sample rate and buffer period size are known.
 *****************************************************************************/
void
init_sync_info(unsigned int sample_rate, unsigned short period_size)
{
	unsigned short     period;

	for (period = 0; period < MAX_BUFFER_PERIODS; period++) {
		sync_info[period].jack_wakeup_frame         = 0;
		sync_info[period].sample_rate               = sample_rate;
		sync_info[period].f_sample_rate             = (timecalc_t)(sample_rate);
		sync_info[period].start_time.tv_sec         = 0;
		sync_info[period].start_time.tv_nsec        = JAMROUTER_CLOCK_INIT;
		sync_info[period].end_time.tv_sec           = 0;
		sync_info[period].end_time.tv_nsec          = JAMROUTER_CLOCK_INIT;
		sync_info[period].sensing_timeout[A2J_QUEUE].tv_sec    = 0;
		sync_info[period].sensing_timeout[A2J_QUEUE].tv_nsec   =
			JAMROUTER_CLOCK_INIT;
		sync_info[period].sensing_timeout[J2A_QUEUE].tv_sec    = 0;
		sync_info[period].sensing_timeout[J2A_QUEUE].tv_nsec   =
			JAMROUTER_CLOCK_INIT;
		set_new_period_size(period, period_size);
	}
}


/*****************************************************************************
 * set_midi_cycle_time()
 *
 * This function fully manages calculating the timestamps for MIDI period
 * start and end, using a decayed average to maintain a rock-steady MIDI
 * period time interval reference from one period to the next.  This design
 * allows the underlying audio system to wake up and begin its processing
 * early or late without translating thread scheduling jitter into event
 * scheduling and timestamping jitter, as long as there are no xruns and the
 * audio system runs its processing callback sometime within the calculated
 * time interval of the current MIDI period.
 *
 * This function sets the midi period time reference to a regular cycle, using
 * the timing of when this function is called as an incoming clock pulse to
 * generate an absolutely steady phase locked time reference for determining
 * the frame position within the audio buffer of incoming MIDI events.  This
 * function is to be called exactly once for every audio buffer period, and
 * can be called at any time within the processing period.  Basically, this is
 * a fancy software PLL with the incoming clock pulse handled by one thread
 * while another thread checks to see when the stable output clock pulse
 * _would_ have fired, times its events, and updates its buffer index
 * accordingly.  Timing jitter of when this function is called is not a
 * problem as long as the average period time remains relatively stable.  We
 * just need to remember that in the absense of xruns, we assume that an audio
 * processing period is never actually late, just later than we expected.  It
 * can only be early (and always less than 1 full period early).
 *****************************************************************************/
unsigned short
set_midi_cycle_time(unsigned short period, int nframes)
{
	TIMESTAMP               next_timeref;
	TIMESTAMP               timeref;
	TIMESTAMP               last;
	timecalc_t              delta_nsec;
	timecalc_t              avg_period_nsec;
	unsigned short          last_period;
	unsigned short          next_period;

	last_period = period;
	next_period = (unsigned short)(period + 1) &
		sync_info[period].period_mask;

	last.tv_sec  = (int)jack_start_time.tv_sec;
	last.tv_nsec = (int)jack_start_time.tv_nsec;

	clock_gettime(system_clockid, &jack_start_time);

	/* For next period, start with current period's sync_info[] */
	sync_info[next_period].f_buffer_period_size =
		sync_info[period].f_buffer_period_size;
	sync_info[next_period].f_sample_rate        =
		sync_info[period].f_sample_rate;
	sync_info[next_period].sample_rate          =
		sync_info[period].sample_rate;
	sync_info[next_period].buffer_size          =
		sync_info[period].buffer_size;
	sync_info[next_period].buffer_size_mask     =
		sync_info[period].buffer_size_mask;
	sync_info[next_period].buffer_period_size   =
		sync_info[period].buffer_period_size;
	sync_info[next_period].buffer_periods       =
		sync_info[period].buffer_periods;
	sync_info[next_period].period_mask          =
		sync_info[period].period_mask;
	sync_info[next_period].rx_latency_size      =
		sync_info[period].rx_latency_size;
	sync_info[next_period].tx_latency_size      =
		sync_info[period].tx_latency_size;
	sync_info[next_period].rx_latency_periods   =
		sync_info[period].rx_latency_periods;
	sync_info[next_period].tx_latency_periods   =
		sync_info[period].tx_latency_periods;

	delta_nsec = get_delta_nsecs(&jack_start_time,
	                             &(sync_info[last_period].start_time));

	/* Delay between start_midi_clock() and first call to this
	   function is not always determinate, so check for clock init
	   and set timestamp here. */
	if ( (last.tv_nsec == JAMROUTER_CLOCK_INIT) ||
	     (nframes != sync_info[last_period].buffer_period_size) ||
	     (sync_info[last_period].end_time.tv_nsec == JAMROUTER_CLOCK_INIT) ) {

		/* Set buffer period size and calculate new sync variables */
		for (next_period = 0; next_period < MAX_BUFFER_PERIODS; next_period++) {
			/* Don't touch current period.  Other threads are still using the
			   current period's old sync_info[].  They will pick up new
			   sync_info[] during the next period. */
			if (next_period != period) {
				set_new_period_size(next_period, (unsigned short)(nframes));
			}
		}
		next_period =
			(unsigned short)(period + 1) & sync_info[period].period_mask;
		JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
		                DEBUG_COLOR_YELLOW "@" DEBUG_COLOR_DEFAULT);

		sync_info[last_period].nsec_per_period =
			sync_info[last_period].f_buffer_period_size *
			(timecalc_t)1000000000.0 /
			(timecalc_t) sync_info[last_period].sample_rate;
		sync_info[last_period].nsec_per_frame =
			(sync_info[last_period].nsec_per_period /
			 sync_info[last_period].f_buffer_period_size);

		/* Assume the processing thread woke up at the expected phase in the
		   current MIDI period. */
		delta_nsec = sync_info[next_period].nsec_per_frame *
			(timecalc_t)(midi_phase_lock);

		/* Set initial timeref to match target audio wakeup phase. */
		sync_info[last_period].start_time.tv_sec  = jack_start_time.tv_sec;
		sync_info[last_period].start_time.tv_nsec = jack_start_time.tv_nsec;
		time_sub_nsecs(&(sync_info[last_period].start_time),
		               (int)(delta_nsec));

		sync_info[last_period].end_time.tv_sec  = jack_start_time.tv_sec;
		sync_info[last_period].end_time.tv_nsec = jack_start_time.tv_nsec;
		time_add_nsecs(&(sync_info[last_period].end_time),
		               (int)(sync_info[last_period].nsec_per_period -
		                     delta_nsec));

		sync_info[next_period].start_time.tv_sec  = jack_start_time.tv_sec;
		sync_info[next_period].start_time.tv_nsec = jack_start_time.tv_nsec;
		time_add_nsecs(&(sync_info[next_period].start_time),
		               (int)(sync_info[next_period].nsec_per_period -
		                     delta_nsec));

		sync_info[next_period].end_time.tv_sec  = jack_start_time.tv_sec;
		sync_info[next_period].end_time.tv_nsec = jack_start_time.tv_nsec;
		time_add_nsecs(&(sync_info[next_period].end_time),
		               (int)(((timecalc_t)(2.0) *
		                      sync_info[last_period].nsec_per_period)
		                     - delta_nsec));

		/* Set nsec_per_period and nsec_per_frame for all periods except the
		   currently active one. */
		for (next_period = (unsigned short)((last_period + 1) &
		                                    sync_info[period].period_mask);
		     next_period != period;
		     next_period = (unsigned short)((next_period + 1) &
		                                    sync_info[period].period_mask)) {
			sync_info[next_period].nsec_per_period =
				sync_info[next_period].f_buffer_period_size *
				(timecalc_t)(1000000000.0) /
				(timecalc_t)(sync_info[next_period].f_sample_rate);
			sync_info[next_period].nsec_per_frame  =
				(sync_info[next_period].nsec_per_period /
				 sync_info[next_period].f_buffer_period_size);

			last_period = next_period;
		}
		last_period = period;
		next_period = (unsigned short)(period + 1) &
			sync_info[period].period_mask;
	}

	/* handle the normal case (no clock restart). */
	else {
		/* get time in nanoseconds since beginning of MIDI period. */
		delta_nsec = get_delta_nsecs(&jack_start_time,
		                             &(sync_info[last_period].start_time));

		avg_period_nsec   = sync_info[last_period].nsec_per_period;
		avg_period_nsec  -= (avg_period_nsec / (timecalc_t)(2048.0));

		/* handle system clock wrapping around. */
		if ((jack_start_time.tv_sec == 0) && (last.tv_sec != 0)) {
			avg_period_nsec +=
				((timecalc_t)((timecalc_t)(1000000000) +
				              (timecalc_t)(jack_start_time.tv_nsec) -
				              (timecalc_t)(last.tv_nsec)) /
				 (timecalc_t)(2048.0));
		}
		else {
			avg_period_nsec +=
				((timecalc_t)((((timecalc_t)(jack_start_time.tv_sec) -
				                (timecalc_t)(last.tv_sec)) * 1000000000) +
				              ((timecalc_t)(jack_start_time.tv_nsec) -
				               (timecalc_t)(last.tv_nsec))) /
				 (timecalc_t)(2048.0));
		}
		sync_info[next_period].nsec_per_period = avg_period_nsec;
		sync_info[next_period].nsec_per_frame  =
			(sync_info[next_period].nsec_per_period /
			 sync_info[next_period].f_buffer_period_size);

	}

	timeref.tv_sec       = sync_info[last_period].start_time.tv_sec;
	timeref.tv_nsec      = sync_info[last_period].start_time.tv_nsec;
	next_timeref.tv_sec  = timeref.tv_sec;
	next_timeref.tv_nsec = timeref.tv_nsec;

	sync_info[period].jack_wakeup_frame =
		(signed short)(delta_nsec / sync_info[period].nsec_per_frame);

	/* Latch the clock when audio wakes up before the calculated midi period
	   start. coming in one frame too early is unfortunately common with
	   non-rt kernels or missing realtime priveleges.  allow for an extra
	   frame. */
	if (delta_nsec < (sync_info[period].nsec_per_frame)) {
		next_timeref.tv_sec   = (int) jack_start_time.tv_sec;
		next_timeref.tv_nsec  = (int) jack_start_time.tv_nsec;
		next_timeref.tv_nsec -=
			((int)(sync_info[next_period].nsec_per_period -
			       (sync_info[next_period].nsec_per_frame *
			        (sync_info[next_period].f_buffer_period_size -
			         midi_phase_lock))));
		/* Nudge nsec_per_period in the right direction to speed up clock
		   settling time. */
		sync_info[next_period].nsec_per_period -=
			((timecalc_t)(0.015625) * sync_info[next_period].nsec_per_frame);
		JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
		                DEBUG_COLOR_YELLOW "|||%d|||--- " DEBUG_COLOR_DEFAULT,
		                sync_info[period].jack_wakeup_frame);
	}
	/* Half frame jitter correction for audio waking up too early, but within
	   the current midi period. */
	else if (delta_nsec <
	         (sync_info[period].nsec_per_frame * midi_phase_min)) {
		next_timeref.tv_nsec -=
			((int)(sync_info[next_period].nsec_per_frame * 0.5));
		JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
		                DEBUG_COLOR_CYAN "<<" DEBUG_COLOR_BLUE "%d"
		                DEBUG_COLOR_CYAN "<< " DEBUG_COLOR_DEFAULT,
		                sync_info[period].jack_wakeup_frame);
	}
	/* This condition is reached when the phase is locked. */
	else if (delta_nsec <
	         (sync_info[period].nsec_per_frame * midi_phase_max)) {
		JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
		                DEBUG_COLOR_BLUE "%d " DEBUG_COLOR_DEFAULT,
		                sync_info[period].jack_wakeup_frame);
	}
	/* Half frame jitter correction for audio waking up too late, but within
	   the current midi period. */
	else if ( (delta_nsec < (sync_info[period].nsec_per_period)) &&
	          (sync_info[period].jack_wakeup_frame <
	           sync_info[period].f_buffer_period_size) ) {
		next_timeref.tv_nsec +=
			((int)(sync_info[next_period].nsec_per_frame * 0.5));
		JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
		                DEBUG_COLOR_CYAN ">>" DEBUG_COLOR_BLUE "%d"
		                DEBUG_COLOR_CYAN ">> " DEBUG_COLOR_DEFAULT,
		                sync_info[period].jack_wakeup_frame);
	}
	/* Latch the clock when audio wakes up after the calculated period end. */
	else {
		next_timeref.tv_nsec +=
			(int)(delta_nsec - (sync_info[next_period].nsec_per_frame * 0.5 *
			                    (sync_info[next_period].f_buffer_period_size -
			                     1.0 + midi_phase_max)));
		/* Nudge nsec_per_period in the right direction to speed up clock
		   settling time. */
		sync_info[next_period].nsec_per_period +=
			((timecalc_t)(0.015625) * sync_info[next_period].nsec_per_frame);
		JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
		                DEBUG_COLOR_YELLOW "+++|||%d||| " DEBUG_COLOR_DEFAULT,
		                sync_info[period].jack_wakeup_frame);
	}

	/* Advance the timeref by one period for start of the next. */
	time_add_nsecs(&next_timeref,
	               (int)(sync_info[next_period].nsec_per_period));
	sync_info[next_period].start_time.tv_sec  = next_timeref.tv_sec;
	sync_info[next_period].start_time.tv_nsec = next_timeref.tv_nsec;

	/* Advance the timeref by another period for end of the next. */
	time_add_nsecs(&next_timeref,
	               (int)(sync_info[next_period].nsec_per_period));
	sync_info[next_period].end_time.tv_sec    = next_timeref.tv_sec;
	sync_info[next_period].end_time.tv_nsec   = next_timeref.tv_nsec;

	/* Use the same timeref for the start of the period after the next. */
	next_period = (unsigned short)(next_period + 1) &
		sync_info[period].period_mask;
	sync_info[next_period].start_time.tv_sec  = next_timeref.tv_sec;
	sync_info[next_period].start_time.tv_nsec = next_timeref.tv_nsec;

	/* Advance by one period for end of the period after the next. */
	time_add_nsecs(&next_timeref,
	               (int)(sync_info[next_period].nsec_per_period));
	sync_info[next_period].end_time.tv_sec  = next_timeref.tv_sec;
	sync_info[next_period].end_time.tv_nsec = next_timeref.tv_nsec;

	/* return the next period index back to the caller */
	next_period = (unsigned short)(period + 1) &
		sync_info[period].period_mask;

	/* sync_info[] is now set for the next period and will not be written to
	   again for one full period.  This memory fence helps ensure that the
	   compiler and the CPU do not do the wrong thing when reordering reads
	   into sync_info[]. */
	asm volatile ("mfence; # read/write fence" : : : "memory");

	return next_period;
}


/*****************************************************************************
 * set_active_sensing_timeout()
 *
 * Called whenever an active sensing message is received (or when any other
 * message is received less than 300ms after an active sensing message).
 *****************************************************************************/
void
set_active_sensing_timeout(unsigned short period, unsigned char queue_num)
{
	TIMESTAMP     now;

	if (clock_gettime(system_clockid, &(now)) == 0) {
		sync_info[period].sensing_timeout[queue_num].tv_sec  =
			(int)now.tv_sec;
		sync_info[period].sensing_timeout[queue_num].tv_nsec =
			(int)now.tv_nsec;
		time_add_nsecs(&(sync_info[period].sensing_timeout[queue_num]),
		               300000000);
		JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
		             DEBUG_COLOR_YELLOW "<A> " DEBUG_COLOR_DEFAULT);
	}
}


/*****************************************************************************
 * check_active_sensing_timeout()
 *
 * Checks for a current active sensing timeout and if one is found,
 * its current status is returned.
 *****************************************************************************/
int
check_active_sensing_timeout(unsigned short period, unsigned char queue_num)
{
	if ( (sync_info[period].sensing_timeout[queue_num].tv_sec != 0) &&
	     (sync_info[period].sensing_timeout[queue_num].tv_nsec != 0) ) {

		if (timecmp(&(sync_info[period].sensing_timeout[queue_num]),
		            &(sync_info[period].end_time), TIME_LE)) {

			sync_info[period].sensing_timeout[queue_num].tv_sec  = 0;
			sync_info[period].sensing_timeout[queue_num].tv_nsec = 0;

			JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
			             DEBUG_COLOR_YELLOW "<Z> " DEBUG_COLOR_DEFAULT);
			return ACTIVE_SENSING_STATUS_TIMEOUT;
		}
		return ACTIVE_SENSING_STATUS_TIMER_PRESENT;
	}
	return ACTIVE_SENSING_STATUS_NO_TIMER_PRESENT;
}
