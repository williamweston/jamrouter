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
#ifndef WITHOUT_JACK_DLL
#include <jack.h>
#endif
#include "jamrouter.h"
#include "time.h"
#include "timekeeping.h"
#include "mididefs.h"
#include "midi_event.h"
#include "debug.h"
#include "driver.h"


volatile SYNC_INFO      sync_info[MAX_BUFFER_PERIODS];

struct timespec         jack_start_time       = { 0, JAMROUTER_CLOCK_INIT };

timecalc_t              midi_phase_lock       = 0;
timecalc_t              midi_phase_min        = 1.0;
timecalc_t              midi_phase_max        = 127.0;
timecalc_t              setting_midi_phase_lock = DEFAULT_MIDI_PHASE_LOCK;
timecalc_t              decay_generations     = 2048.0;

int                     max_event_latency     = 0;


/*****************************************************************************
 * sleep_until_next_period()
 *
 * TODO:  Consider the use of select() to sleep for a maximum time instead
 *        a minimum time as with usleep() and clock_nanosleep().
 *****************************************************************************/
unsigned short
sleep_until_next_period(unsigned short period, TIMESTAMP *now)
{
	TIMESTAMP           sleep_time;

	if ( (clock_gettime(system_clockid, now) == 0) &&
	     timecmp(now, &(sync_info[period].end_time), TIME_LT) ) {
		time_copy(&sleep_time, &(sync_info[period].end_time));
		time_sub(&sleep_time, now);
#ifdef HAVE_CLOCK_NANOSLEEP
		clock_nanosleep(CLOCK_MONOTONIC, 0, &sleep_time, NULL);
#else
		nanosleep(sleep_time);
#endif
	}

	//period++;
	//period &= sync_info[period].period_mask;
	period = sync_info[period].next;

	sync_info[period].jack_wakeup_frame = 0;

	JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
	                DEBUG_COLOR_GREEN "%d! " DEBUG_COLOR_DEFAULT, period);

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
	TIMESTAMP sleep_time;

	time_copy(&sleep_time, &(sync_info[period].start_time));
	time_add_nsecs(&sleep_time,
	               (int)(sync_info[period].nsec_per_frame *
	                     (timecalc_t)(frame)));

	if ( (clock_gettime(system_clockid, &now) == 0) &&
	     (timecmp(&now, &sleep_time, TIME_LT) ) ) {
		time_sub(&sleep_time, &now);
#ifdef HAVE_CLOCK_NANOSLEEP
		clock_nanosleep(CLOCK_MONOTONIC, 0, &sleep_time, NULL);
#else
		nanosleep(sleep_time);
#endif
	}
}


/*****************************************************************************
 * set_midi_phase_lock
 *****************************************************************************/
void
set_midi_phase_lock(unsigned short period)
{
#if 0
	midi_phase_lock = 6.0;
	midi_phase_min  = midi_phase_lock - (timecalc_t)(3.0);
	midi_phase_max  = midi_phase_lock + (timecalc_t)(3.0);
#else
	midi_phase_lock = (timecalc_t)(setting_midi_phase_lock *
	                               sync_info[period].f_buffer_period_size);

	/* If not changed from the default, set phase lock to 0.75 at 1-period rx
	   latency for 48000/64, 96000/128, 192000/256, etc. rather than enforcing
	   a minimum of 2 rx latency periods for these settings.
	   TODO:  Check to see if this is still necessary. */
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
		midi_phase_min   = (timecalc_t)(1.5);
		midi_phase_max   = (timecalc_t)(8.5);
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

		midi_phase_min  = midi_phase_lock - (timecalc_t)(4.0);
		midi_phase_max  = midi_phase_lock + (timecalc_t)(4.0);
	}
#endif
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
	TIMESTAMP           now;
	unsigned char       period = 0;

	time_init(&jack_start_time, JAMROUTER_CLOCK_INIT);

	sync_info[period].nsec_per_period =
		sync_info[period].f_buffer_period_size *
		(timecalc_t)(1000000000.0) /
		(timecalc_t)(sync_info[period].sample_rate);

	sync_info[period].nsec_per_frame =
		sync_info[period].nsec_per_period
		/ sync_info[period].f_buffer_period_size;

	//set_midi_phase_lock(period);

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
			time_copy(&(sync_info[period].start_time), &now);
			/* initialize the active sensing timeout to zero (off). */
			time_init(&(sync_info[period].sensing_timeout[A2J_QUEUE]),
			          JAMROUTER_CLOCK_INIT);
			time_init(&(sync_info[period].sensing_timeout[J2A_QUEUE]),
			          JAMROUTER_CLOCK_INIT);
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
	   fetched, either a true CPU cache latency miss or the audio system
	   skipping a cycle, extra logic is included below so that JAMRouter MIDI
	   threads do not do the wrong thing when it comes to synchronization. */
	asm volatile ("mfence; # read/write fence" : : : "memory");

	for (period = 0; period < MAX_BUFFER_PERIODS; period++) {
		/* check timestamps to find current MIDI period */
		if ( ( timecmp(now, &(sync_info[last_period].end_time), TIME_GT) ||
		       timecmp(now, &(sync_info[period].start_time),    TIME_GE) ) &&
		     timecmp(now, &(sync_info[period].end_time),    TIME_LE) ) {
			return (unsigned short)(period);
		}
		/* keep track of most recent sync_info found. */
		if ( timecmp(&recent, &(sync_info[period].start_time),    TIME_LE) ||
		     timecmp(&recent, &(sync_info[last_period].end_time), TIME_LT) ) {
			time_copy(&recent, &(sync_info[period].start_time));
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
       late.  This of course should never be a concern during the middle of a
       performance. */
	time_copy(&delta, now);
	time_sub(&delta, &recent);
	delta_nsec = time_nsecs(&delta);
	if (sync_info[recent_period].nsec_per_period != 0.0) {
		elapsed_periods =
			(unsigned short)(delta_nsec /
			                 sync_info[recent_period].nsec_per_period);
		period =
			(unsigned short)((recent_period + elapsed_periods) &
			                 sync_info[recent_period].period_mask);
		JAMROUTER_DEBUG(DEBUG_CLASS_TESTING, DEBUG_COLOR_RED "}}%d{{ ", period);
	}
	else {
		JAMROUTER_DEBUG(DEBUG_CLASS_TESTING, DEBUG_COLOR_RED "}}{{ ", period);
	}

	return period;
}


/*****************************************************************************
 * get_midi_frame()
 *
 * Returns the frame number corresponding to the supplied period and
 * timestamp.  Processing flags are:
 *
 * FRAME_TIMESTAMP:    Obtain a new timestamp, overwriting old.
 * FRAME_LIMIT_LOWER:  Enforce zero as the lower frame limit.
 * FRAME_LIMIT_UPPER:  Enforce buffer period size minus one as upper limit.
 * FRAME_FIX_LOWER:    Negative values are indicative of the sync_info[]
 *   ringbuffer not being updated for the current thread, most likely due to
 *   the audio system skipping a cycle or old values being stuck in the CPU
 *   cache.  This option detects stale sync_info[] and adjusts the period and
 *   frame accordingly.  At most buffer_size/sample_rate combinations, this is
 *   either not possible or an extremely rare corner case.  At 16/96000, this
 *   extra logic becomes an absolute necessity for rock-solid timing.  In all
 *   cases, this logic has been observed to make the proper correction 100% of
 *   the time.
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
				*period = sync_info[*period].prev;
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
	time_copy(frame_time, &(sync_info[period].start_time));
	time_add_nsecs(frame_time, (int)(sync_info[period].nsec_per_frame *
	                                 (timecalc_t)(frame)));

	return frame_time;
}


/*****************************************************************************
 * get_delta_nsecs()
 *
 * Returns the floating point difference in nanoseconds between
 * two timestamps (usually now and process callback start time).
 *****************************************************************************/
timecalc_t
get_delta_nsecs(TIMESTAMP *now, volatile TIMESTAMP *start)
{
	if ((now->tv_sec == 0) && ((now->tv_nsec == 0) ||
	                           (now->tv_nsec == JAMROUTER_CLOCK_INIT))) {
		if (clock_gettime(system_clockid, now) != 0) {
			jamrouter_shutdown("clock_gettime() failed!\n");
		}
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
	unsigned short  last_period;
	unsigned short  p;

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
		/* ignore extra command line latencies for larger buffer sizes. */
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
	sync_info[period].buffer_period_mask   =
		(unsigned short)(sync_info[period].buffer_period_size - 1);
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

	/* set frames per byte for latency calculation and jitter correction. */
	switch (sync_info[period].sample_rate) {
	case 22050:
		sync_info[period].frames_per_byte = 8;
		break;
	case 32000:
		sync_info[period].frames_per_byte = 10;
		break;
	case 44100:
		sync_info[period].frames_per_byte = 15;
		break;
	case 48000:
		sync_info[period].frames_per_byte = 16;
		break;
	case 64000:
		sync_info[period].frames_per_byte = 20;
		break;
	case 88200:
	case 96000:
		sync_info[period].frames_per_byte = 30;
		break;
	case 176400:
	case 192000:
		sync_info[period].frames_per_byte = 60;
		break;
	case 384000:
		sync_info[period].frames_per_byte = 120;
		break;
	default:
		sync_info[period].frames_per_byte = (short)
			((sync_info[period].sample_rate * 10 ) / 31250);
	}

	/* Since the decayed average is integrated at higher frequencies with
	   lower buffer sizes and higher sampling rates, compensate here for
	   stability.  This is tunable in seconds and provides nearly the same
	   drift vs. time characteristics at all buffer size and sample rate
	   combinations. */
	decay_generations = (timecalc_t)(360.0) *
		sync_info[period].f_sample_rate /
		sync_info[period].f_buffer_period_size;

	/* calculate new midi phase lock for current buffer size. */
	set_midi_phase_lock(period);

	/* rebuild linked list based on number of periods */
	last_period = sync_info[period].period_mask;
	for (p = 0;
	     p < sync_info[period].buffer_periods;
	     p++) {
		sync_info[p].prev = last_period;
		sync_info[last_period].next = p;
		last_period = p;
	}
	for (p = sync_info[period].buffer_periods;
	     p < MAX_BUFFER_PERIODS;
	     p++) {
		sync_info[p].prev = last_period;
		sync_info[last_period].next = 0;
		last_period = p;
	}
}


/*****************************************************************************
 * init_sync_info()
 *
 * Called once after audio system initialization to initialize the sync_info[]
 * ringbuffer once the sample rate and buffer period size are known.
 *****************************************************************************/
void
init_sync_info(unsigned int sample_rate, unsigned short UNUSED(period_size))
{
	unsigned short     period;
	unsigned short     last_period  = MAX_BUFFER_PERIODS - 1;

	for (period = 0; period < MAX_BUFFER_PERIODS; period++) {
		sync_info[period].jack_wakeup_frame  = 0;
		sync_info[period].jack_frames        = 0;
		sync_info[period].jack_current_usecs = 0;
		sync_info[period].jack_next_usecs    = 0;
		sync_info[period].sample_rate        = sample_rate;
		sync_info[period].f_sample_rate      = (timecalc_t)(sample_rate);
		sync_info[period].start_time.tv_sec  = 0;
		sync_info[period].start_time.tv_nsec = JAMROUTER_CLOCK_INIT;
		sync_info[period].end_time.tv_sec    = 0;
		sync_info[period].end_time.tv_nsec   = JAMROUTER_CLOCK_INIT;
		time_init(&(sync_info[period].sensing_timeout[A2J_QUEUE]),
		          JAMROUTER_CLOCK_INIT);
		time_init(&(sync_info[period].sensing_timeout[J2A_QUEUE]),
		          JAMROUTER_CLOCK_INIT);
		sync_info[period].prev = last_period;
		sync_info[last_period].next = period;
		last_period = period;
	}
	sync_info[3].next = 0;
	sync_info[0].prev = 3;
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
 * _would_ have fired, times its events, etc.  Timing jitter of when this
 * function is called is not a problem as long as the average period time
 * remains relatively stable.  We just need to remember that in the absense of
 * xruns, we assume that an audio processing period is never actually late,
 * just later than we expected.  It can only be early (and always less than 1
 * full period early).
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
#ifndef WITHOUT_JACK_DLL
	/* these values are provided by jack_get_cycle_times() */
	jack_nframes_t          current_frames;
	jack_time_t             current_usecs;
	jack_time_t             next_usecs;
	float                   period_usecs;
#endif
	jack_nframes_t          cycle_elapsed   = 0;

	last_period = period;
	next_period = (unsigned short)(period + 1) &
		sync_info[period].period_mask;

	time_copy(&last, &jack_start_time);

	clock_gettime(system_clockid, &jack_start_time);

#ifndef WITHOUT_JACK_DLL
	if ( (jack_dll_level > 0) &&
	     (jack_get_cycle_times(jack_audio_client,
	                           &current_frames, &current_usecs,
	                           &next_usecs, &period_usecs) == 0) ) {

		sync_info[next_period].jack_frames        = current_frames;
		sync_info[next_period].jack_current_usecs = current_usecs;
		sync_info[next_period].jack_next_usecs    = next_usecs;

		sync_info[next_period].jack_nsec_per_period = (timecalc_t)
			((unsigned int)(sync_info[next_period].jack_next_usecs) -
			 (unsigned int)(sync_info[next_period].jack_current_usecs)) *
			(timecalc_t)(1000.0);

		sync_info[next_period].jack_nsec_per_frame =
			sync_info[next_period].jack_nsec_per_period /
			sync_info[next_period].f_buffer_period_size;

		cycle_elapsed = jack_frames_since_cycle_start(jack_audio_client);
		time_sub_nsecs(&jack_start_time,
		               (int)((timecalc_t)(cycle_elapsed) *
		                     sync_info[next_period].jack_nsec_per_period));
	}
#endif

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
	sync_info[next_period].buffer_period_mask   =
		sync_info[period].buffer_period_mask;
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
			/* Don't touch current period other than fixing the indexes.
			   Other threads are still using the current period's old
			   sync_info[].  They will pick up new sync_info[] during the next
			   period. */
			if (next_period != period) {
				set_new_period_size(next_period, (unsigned short)(nframes));
			}
		}
		next_period = sync_info[period].next;

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
		time_copy(&(sync_info[last_period].start_time), &jack_start_time);
		time_sub_nsecs(&(sync_info[last_period].start_time),
		               (int)(delta_nsec));

		time_copy(&(sync_info[last_period].end_time), &jack_start_time);
		time_add_nsecs(&(sync_info[last_period].end_time),
		               (int)(sync_info[last_period].nsec_per_period -
		                     delta_nsec));

		time_copy(&(sync_info[next_period].start_time), &jack_start_time);
		time_add_nsecs(&(sync_info[next_period].start_time),
		               (int)(sync_info[next_period].nsec_per_period -
		                     delta_nsec));

		time_copy(&(sync_info[next_period].end_time), &jack_start_time);
		time_add_nsecs(&(sync_info[next_period].end_time),
		               (int)(((timecalc_t)(2.0) *
		                      sync_info[last_period].nsec_per_period)
		                     - delta_nsec));

		/* Set nsec_per_period and nsec_per_frame for all periods except the
		   currently active one. */
		for (next_period = sync_info[period].next;
		     next_period != period;
		     next_period = sync_info[next_period].next) {

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
		next_period = sync_info[period].next;
	}

	/* handle the normal case (no clock restart). */
	else {
		/* get time in nanoseconds since beginning of MIDI period. */
		delta_nsec = get_delta_nsecs(&jack_start_time,
		                             &(sync_info[last_period].start_time));

		avg_period_nsec   = sync_info[last_period].nsec_per_period;
		avg_period_nsec  -=
			(avg_period_nsec / decay_generations);

		/* handle system clock wrapping around. */
		if ((jack_start_time.tv_sec == 0) && (last.tv_sec != 0)) {
			avg_period_nsec +=
				((timecalc_t)((timecalc_t)(1000000000.0) +
				              (timecalc_t)(jack_start_time.tv_nsec) -
				              (timecalc_t)(last.tv_nsec)) /
				 decay_generations);
		}
		else {
			avg_period_nsec +=
				((timecalc_t)((((timecalc_t)(jack_start_time.tv_sec) -
				                (timecalc_t)(last.tv_sec)) * 1000000000.0) +
				              ((timecalc_t)(jack_start_time.tv_nsec) -
				               (timecalc_t)(last.tv_nsec))) /
				 decay_generations);
		}
		sync_info[next_period].nsec_per_period = avg_period_nsec;
		sync_info[next_period].nsec_per_frame  =
			(sync_info[next_period].nsec_per_period /
			 sync_info[next_period].f_buffer_period_size);

		/* TODO:  Turn this into a higher order filter by adding either
		   additional decayed average stages or integrators tuned at different
		   time intervals, with the first stage only being the one that
		   compensates for buffer period size and sample rate. */
	}

	time_copy(&timeref, &(sync_info[last_period].start_time));
	time_copy(&next_timeref, &timeref);

	sync_info[period].jack_wakeup_frame =
		(signed short)(delta_nsec / sync_info[period].nsec_per_frame);

#ifdef EXTRA_DEBUG
	if ((jack_dll_level > 0) && (debug_class & DEBUG_CLASS_ANALYZE)) {
		//JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
		//                DEBUG_COLOR_LTBLUE "{%d:%d:%d:%f} "
		//                DEBUG_COLOR_DEFAULT,
		//                current_frames - sync_info[period].jack_frames,
		//                current_usecs, next_usecs, period_usecs);
		if ( (current_frames - sync_info[period].jack_frames) !=
		     sync_info[period].buffer_period_size) {
			JAMROUTER_DEBUG(DEBUG_CLASS_ANALYZE,
			                DEBUG_COLOR_RED "{%u} "
			                DEBUG_COLOR_DEFAULT,
			                current_frames - sync_info[period].jack_frames);
		}
	}
#endif

#if 0
	/* Latch the clock when audio wakes up before the calculated midi period
	   start. coming in one frame too early is unfortunately common with
	   non-rt kernels or missing realtime priveleges.  allow for an extra
	   frame. */
	if (delta_nsec < (sync_info[period].nsec_per_frame)) {
		next_timeref.tv_sec   = (int)jack_start_time.tv_sec;
		next_timeref.tv_nsec  = (int)jack_start_time.tv_nsec;
		next_timeref.tv_nsec -=
			((int)(sync_info[next_period].nsec_per_period -
			       (sync_info[next_period].nsec_per_frame *
			        (sync_info[next_period].f_buffer_period_size -
			         midi_phase_lock))));
		/* Reset nsec_per_frame and nsec_per_period for quick clock resettling
		   times after xruns or other events that throw off the audio process
		   thread's scheduling. */
		sync_info[next_period].nsec_per_frame =
			(timecalc_t)(1000000000.0) / sync_info[period].f_sample_rate;
		sync_info[next_period].nsec_per_period =
			sync_info[period].f_buffer_period_size * (timecalc_t)(1000000000.0) /
			sync_info[period].f_sample_rate;
		JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
		                DEBUG_COLOR_YELLOW "|||%d%+d|||--- " DEBUG_COLOR_DEFAULT,
		                sync_info[period].jack_wakeup_frame, cycle_elapsed);
	}
	/* Phase locking lower bound: subtract a partial frame from the start of
	   the next period (no more than 0.5 to maintian continuity). */
	else
#endif
	if (delta_nsec <
		    (sync_info[period].nsec_per_frame * midi_phase_min)) {
		time_sub_nsecs(&next_timeref,
		               ((int)(sync_info[next_period].nsec_per_frame * 0.25)));
		JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
		                DEBUG_COLOR_LTBLUE "<<" DEBUG_COLOR_BLUE "%d%+d"
		                DEBUG_COLOR_LTBLUE "<< " DEBUG_COLOR_DEFAULT,
		                sync_info[period].jack_wakeup_frame, cycle_elapsed);
	}
	/* This condition is reached when the phase is locked. */
	else if (delta_nsec <
	         (sync_info[period].nsec_per_frame * midi_phase_max)) {
		JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
		                DEBUG_COLOR_BLUE "%d%+d " DEBUG_COLOR_DEFAULT,
		                sync_info[period].jack_wakeup_frame, cycle_elapsed);
	}
	/* Phase locking upper bound: add a partial frame to the start of
	   the next period (no more than 0.5 to maintian continuity). */
	else if ( (delta_nsec < (sync_info[period].nsec_per_period)) &&
	          (sync_info[period].jack_wakeup_frame <
	           sync_info[period].f_buffer_period_size) ) {
		time_add_nsecs(&next_timeref,
		               ((int)(sync_info[next_period].nsec_per_frame * 0.25)));
		JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
		                DEBUG_COLOR_LTBLUE ">>" DEBUG_COLOR_BLUE "%d%+d"
		                DEBUG_COLOR_LTBLUE ">> " DEBUG_COLOR_DEFAULT,
		                sync_info[period].jack_wakeup_frame, cycle_elapsed);
	}
	/* Latch the clock when audio wakes up after the calculated period end. */
	else {
		time_add_nsecs(&next_timeref, (int)
		               ((sync_info[next_period].nsec_per_frame * 
		                 (sync_info[next_period].f_buffer_period_size - 1.0))));
		/* Reset nsec_per_frame and nsec_per_period for quick clock resettling
		   times after xruns or other events that throw off the audio process
		   thread's scheduling. */
		sync_info[next_period].nsec_per_frame =
			(timecalc_t)(1000000000.0) / sync_info[period].f_sample_rate;
		sync_info[next_period].nsec_per_period =
			sync_info[period].f_buffer_period_size * (timecalc_t)(1000000000.0) /
			sync_info[period].f_sample_rate;
		JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
		                DEBUG_COLOR_YELLOW "+++|||%d%+d||| " DEBUG_COLOR_DEFAULT,
		                sync_info[period].jack_wakeup_frame, cycle_elapsed);
	}
    /* Advance the timeref by one period for start of the next. */
	time_add_nsecs(&next_timeref,
	               (int)(sync_info[next_period].nsec_per_period));
	time_copy(&(sync_info[next_period].start_time), &next_timeref);

	/* Advance the timeref by another period for end of the next. */
	time_add_nsecs(&next_timeref,
	               (int)(sync_info[next_period].nsec_per_period));
	time_copy(&(sync_info[next_period].end_time), &next_timeref);

	/* Use the same timeref for the start of the period after the next. */
	next_period = sync_info[next_period].next;
	time_copy(&(sync_info[next_period].start_time), &next_timeref);

	/* Advance by one period for end of the period after the next. */
	time_add_nsecs(&next_timeref,
	               (int)(sync_info[next_period].nsec_per_period));
	time_copy(&(sync_info[next_period].end_time), &next_timeref);

#ifndef WITHOUT_JACK_DLL
	sync_info[next_period].jack_frames =
		current_frames + sync_info[next_period].buffer_period_size;
	sync_info[next_period].jack_current_usecs = next_usecs;
	sync_info[next_period].jack_next_usecs    =
		(next_usecs - current_usecs) + next_usecs;

	//sync_info[next_period].jack_frames = current_frames +
	//	sync_info[next_period].buffer_period_size +
	//	sync_info[next_period].buffer_period_size;
	//sync_info[next_period].jack_current_usecs =
	//	(next_usecs - current_usecs) + next_usecs;
	//sync_info[next_period].jack_next_usecs    =
	//	(next_usecs - current_usecs) +
	//	(next_usecs - current_usecs) + next_usecs;
#endif

	/* return the next period index back to the caller */
	next_period = sync_info[period].next;

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
		time_copy(&(sync_info[period].sensing_timeout[queue_num]), &now);
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
			time_init(&(sync_info[period].sensing_timeout[queue_num]), 0);
			JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
			             DEBUG_COLOR_YELLOW "<Z> " DEBUG_COLOR_DEFAULT);
			return ACTIVE_SENSING_STATUS_TIMEOUT;
		}
		return ACTIVE_SENSING_STATUS_TIMER_PRESENT;
	}
	return ACTIVE_SENSING_STATUS_NO_TIMER_PRESENT;
}
