/*****************************************************************************
 *
 * jack_dll.c
 *
 * JAMRouter:  JACK <--> ALSA MIDI Router
 *
 * Copyright (C) 2015 William Weston <william.h.weston@gmail.com>
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
#include <stdlib.h>
#include <jack/jack.h>
#include "jamrouter.h"
#include "time.h"
#include "timekeeping.h"
#include "jack.h"
#include "jack_dll.h"
#include "debug.h"
#include "driver.h"


/*****************************************************************************
 * get_midi_period_jack_dll()
 *****************************************************************************/
unsigned short
get_midi_period_jack_dll(jack_nframes_t current_frame)
{
	unsigned short     period;
	unsigned short     elapsed_periods;
	unsigned short     recent_period        = 0;
	jack_nframes_t     recent_cycle_start   = 0;
	jack_nframes_t     delta_frames;
	
	/* Any thread needing to know what period we are in right now should under
	   no circumstances use invalid sync_info[] for the current period.  This
	   memory fence assures us that the compiler and the CPU do not do the
	   wrong thing here.  In the case where current sync_info[] is not
	   fetched, either a true CPU cache latency miss or the audio system
	   skipping a cycle, extra logic is included below so that JAMRouter MIDI
	   threads do not do the wrong thing when it comes to synchronization. */
	asm volatile ("mfence; # read/write fence" : : : "memory");

	for (period = 0; period < sync_info[0].buffer_periods; period++) {
		/* check jack framestamps */
		if ( (current_frame >= sync_info[period].jack_frames) &&
		     (current_frame < (sync_info[period].jack_frames +
		                       sync_info[period].buffer_period_size)) ) {
			//JAMROUTER_DEBUG(DEBUG_CLASS_TESTING,
			//                DEBUG_COLOR_RED "}%d{ " DEBUG_COLOR_DEFAULT,
			//                period);
			return period;
		}
		/* keep track of most recent sync_info found. */
		if (sync_info[period].jack_frames > recent_cycle_start) {
			recent_cycle_start = sync_info[period].jack_frames;
			recent_period = period;
		}
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
	delta_frames = current_frame - recent_cycle_start;
	if (sync_info[recent_period].jack_nsec_per_period != 0.0) {
		elapsed_periods =
			(unsigned short)((timecalc_t)(delta_frames) / (timecalc_t)
			                 (sync_info[recent_period].buffer_period_size));
		period =
			(unsigned short)((recent_period + elapsed_periods) &
			                 sync_info[recent_period].period_mask);
	}

	//JAMROUTER_DEBUG(DEBUG_CLASS_TESTING, DEBUG_COLOR_RED "}}%d{{ ", period);

	return period;
}


/*****************************************************************************
 * get_midi_frame_jack_dll()
 *****************************************************************************/
void
get_midi_frame_jack_dll(unsigned short *period, unsigned short *frame)
{
	jack_nframes_t  jack_frame;

	jack_frame = jack_frame_time(jack_audio_client);
	*period = get_midi_period_jack_dll(jack_frame);

	jack_frame = (unsigned int)
		((jack_frame - (unsigned int)(sync_info[*period].jack_frames)) -
		 (unsigned int)(sync_info[*period].buffer_period_size) -
		 (unsigned int)(sync_info[*period].rx_latency_size));

	*frame = (unsigned short)
		(jack_frame & (jack_nframes_t)(sync_info[*period].buffer_period_mask));

	//JAMROUTER_DEBUG(DEBUG_CLASS_ANALYZE,
	//                DEBUG_COLOR_MAGENTA "[%u] " DEBUG_COLOR_DEFAULT,
	//                *frame);
}


/*****************************************************************************
 * get_frame_time_jack_dll()
 *
 * Fills and returns the supplied timestamp with the time corresponding to
 * the supplied frame offset within the supplied period.
 *****************************************************************************/
TIMESTAMP *
get_frame_time_jack_dll(unsigned short   period,
                        unsigned short   frame,
                        TIMESTAMP        *frame_time)
{
	jack_time_t     jack_time;

	jack_time = jack_frames_to_time(jack_audio_client, (jack_nframes_t)
	                                (frame + sync_info[period].jack_frames));

	frame_time->tv_sec  = (int)(jack_time / (jack_time_t)(1000000));
	frame_time->tv_nsec =
		(int)(jack_time % (jack_time_t)(1000000)) * (int)(1000);

	return frame_time;
}


/*****************************************************************************
 * sleep_until_next_period_jack_dll()
 *
 * TODO:  Consider the use of select() to sleep for a maximum time instead
 *        a minimum time as with usleep() and clock_nanosleep().
 *****************************************************************************/
unsigned short
sleep_until_next_period_jack_dll(unsigned short period, TIMESTAMP *now)
{
	TIMESTAMP           sleep_time;
	unsigned short      next_period = sync_info[period].next;

	sleep_time.tv_sec  = (time_t)
		((sync_info[next_period].jack_next_usecs / 1000000UL) + 0);
	sleep_time.tv_nsec = (time_t)
		((sync_info[next_period].jack_next_usecs % 1000000UL) * 1000UL);
	time_add_nsecs(&sleep_time, sync_info[period].jack_error_nsecs);

	if (clock_gettime(system_clockid, now) == 0) {
		//JAMROUTER_DEBUG(DEBUG_CLASS_ANALYZE,
		//                DEBUG_COLOR_YELLOW "<%d.%09d> " DEBUG_COLOR_DEFAULT,
		//                now->tv_sec, now->tv_nsec);

		//JAMROUTER_DEBUG(DEBUG_CLASS_ANALYZE,
		//                DEBUG_COLOR_CYAN "<%d.%09d> " DEBUG_COLOR_DEFAULT,
		//                sleep_time.tv_sec, sleep_time.tv_nsec);

		if (timecmp(now, &sleep_time, TIME_LT)) {
			time_sub(&sleep_time, now);
#ifdef HAVE_CLOCK_NANOSLEEP
			clock_nanosleep(CLOCK_MONOTONIC, 0, &sleep_time, NULL);
#else
			nanosleep(sleep_time);
#endif
		}

		period = sync_info[period].next;

		sync_info[period].jack_wakeup_frame = 0;

		JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
		                DEBUG_COLOR_YELLOW "%d! " DEBUG_COLOR_DEFAULT,
		                period);
	}
	else {
		time_init(&sleep_time, 0);
		time_add_nsecs(&sleep_time, sync_info[period].jack_nsec_per_period);
#ifdef HAVE_CLOCK_NANOSLEEP
			clock_nanosleep(CLOCK_MONOTONIC, 0, &sleep_time, NULL);
#else
			nanosleep(sleep_time);
#endif
		JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
		                DEBUG_COLOR_MAGENTA "%d! " DEBUG_COLOR_DEFAULT,
		                period);
	}

	/* When debug is disabled, a full memory fence is still needed here,
	   as MIDI Tx is about to dequeue. */
	asm volatile (" mfence; # read/write fence" : : : "memory");

	return period;
}


/*****************************************************************************
 * sleep_until_frame_jack_dll()
 *
 * TODO:  Consider the use of select() to sleep for a maximum time instead
 *        a minimum time as with usleep() and clock_nanosleep().
 *****************************************************************************/
void
sleep_until_frame_jack_dll(unsigned short period, unsigned short frame)
{
	TIMESTAMP now;
	TIMESTAMP sleep_time;
	jack_time_t frame_time;

	frame_time = jack_frames_to_time(jack_audio_client,
	                                 sync_info[period].jack_frames +
	                                 (int)(frame));

	sleep_time.tv_sec  = (frame_time / 1000000);
	sleep_time.tv_nsec = ((frame_time % 1000000) * 1000);

	time_add_nsecs(&sleep_time, (int)(sync_info[period].jack_error_nsecs));
	time_add_nsecs(&sleep_time, (int)(sync_info[period].jack_nsec_per_period));

	if ( (clock_gettime(system_clockid, &now) == 0) &&
	     (timecmp(&now, &sleep_time, TIME_LT) ) ) {
		time_sub(&sleep_time, &now);
		//JAMROUTER_DEBUG(DEBUG_CLASS_ANALYZE,
		//                DEBUG_COLOR_RED "ZZZ " DEBUG_COLOR_DEFAULT);
#ifdef HAVE_CLOCK_NANOSLEEP
		clock_nanosleep(CLOCK_MONOTONIC, 0, &sleep_time, NULL);
#else
		nanosleep(sleep_time);
#endif
	}
}
