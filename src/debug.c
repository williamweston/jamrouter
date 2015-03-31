/*****************************************************************************
 *
 * debug.c
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
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <linux/sched.h>
#include "jamrouter.h"
#include "debug.h"


DEBUG_RINGBUFFER    main_debug_queue;

int                 debug       = 0;
int                 debug_done  = 0;
unsigned long       debug_class = 0;

DEBUG_CLASS         debug_class_list[16] = {
	{ DEBUG_CLASS_NONE,           "none" },
	{ DEBUG_CLASS_INIT,           "init" },
	{ DEBUG_CLASS_DRIVER,         "driver" },
	{ DEBUG_CLASS_STREAM,         "stream" },
	{ DEBUG_CLASS_TIMING,         "timing" },
	{ DEBUG_CLASS_TX_TIMING,      "tx-timing" },
	{ DEBUG_CLASS_MIDI_NOTE,      "note" },
	{ DEBUG_CLASS_MIDI_EVENT,     "event" },
	{ DEBUG_CLASS_SESSION,        "session" },
	{ DEBUG_CLASS_OSS,            "oss" },
	{ DEBUG_CLASS_TESTING,        "testing" },
	{ DEBUG_CLASS_ANALYZE,        "analyze" },
	{ DEBUG_CLASS_FULL,           "full" },
	{ (~0UL),                     NULL }
};


/*****************************************************************************
 * jamrouter_printf()
 *****************************************************************************/
void
jamrouter_printf(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}


/*****************************************************************************
 * jamrouter_warn()
 *****************************************************************************/
void
jamrouter_warn(const char *format, ...)
{
	va_list args;
	guint new_debug_index;

	while (!g_atomic_int_compare_and_exchange(&main_debug_queue.debug_token, 0, 1));
	new_debug_index =
		(guint)(g_atomic_int_add(&(main_debug_queue.insert_index), 1)
		        + 1) & DEBUG_BUFFER_MASK;
	g_atomic_int_set(&main_debug_queue.debug_token, 0);
	va_start(args, format);
	vsnprintf(main_debug_queue.msgs[new_debug_index].msg,
	          DEBUG_MESSAGE_SIZE, format, args);
	va_end(args);
	g_atomic_int_set(&(main_debug_queue.msgs[new_debug_index].status), DEBUG_STATUS_QUEUED);
	g_atomic_int_inc(&(main_debug_queue.write_index));
}


/*****************************************************************************
 * jamrouter_debug()
 *****************************************************************************/
void
jamrouter_debug(unsigned int class, const char *format, ...)
{
	va_list args;
	guint new_debug_index;

	if (debug_class & (class)) {
		while (!g_atomic_int_compare_and_exchange(&main_debug_queue.debug_token, 0, 1));
		new_debug_index =
			(guint)(g_atomic_int_add(&(main_debug_queue.insert_index), 1)
			        + 1) & DEBUG_BUFFER_MASK;
		g_atomic_int_set(&main_debug_queue.debug_token, 0);
		va_start(args, format);
		vsnprintf(main_debug_queue.msgs[new_debug_index].msg,
		          DEBUG_MESSAGE_SIZE, format, args);
		va_end(args);
		g_atomic_int_set(&(main_debug_queue.msgs[new_debug_index].status), DEBUG_STATUS_QUEUED);
		g_atomic_int_inc(&(main_debug_queue.write_index));
	}
}


/*****************************************************************************
 * init_debug_buffers()
 *****************************************************************************/
void
init_debug_buffers(void)
{
	int     j;

	memset(&(main_debug_queue), 0, sizeof(DEBUG_RINGBUFFER));
	main_debug_queue.read_index = 1;
	g_atomic_int_set(&(main_debug_queue.write_index), 1);
	g_atomic_int_set(&(main_debug_queue.insert_index), 0);
	g_atomic_int_set(&(main_debug_queue.debug_token), 0);

	for (j = 0; j < DEBUG_MESSAGE_POOL_SIZE; j++) {
		g_atomic_int_set(&(main_debug_queue.msgs[j].status), DEBUG_STATUS_NONE);
	}
}


/*****************************************************************************
 * jamrouter_debug_iteration()
 *****************************************************************************/
void
output_pending_debug(void)
{
	while ( main_debug_queue.read_index !=
	        (g_atomic_int_get(&(main_debug_queue.write_index))
	         & DEBUG_BUFFER_MASK) ) {
		if ( g_atomic_int_get(&(main_debug_queue.msgs[main_debug_queue.read_index].status))
		     == DEBUG_STATUS_QUEUED ) {
			fprintf(stderr, "%s",
			        main_debug_queue.msgs[main_debug_queue.read_index].msg);
			memset(main_debug_queue.msgs[main_debug_queue.read_index].msg,
			       0, DEBUG_MESSAGE_SIZE);
			g_atomic_int_set(&(main_debug_queue.msgs[main_debug_queue.read_index].status),
			                 DEBUG_STATUS_NONE);;
			main_debug_queue.read_index++;
			main_debug_queue.read_index &= DEBUG_BUFFER_MASK;
		}
		else {
			break;
		}
	}
}


/*****************************************************************************
 * jamrouter_debug_thread()
 *****************************************************************************/
void *
jamrouter_debug_thread(void *UNUSED(arg))
{
	char                thread_name[16];
	struct sched_param  schedparam;
	pthread_t           thread_id;

	/* thread name and scheduling and priority */
	thread_id = pthread_self();
	snprintf(thread_name, 16, "jamrouter%c-dbug", ('0' + jamrouter_instance));
	pthread_setname_np(thread_id, thread_name);
	memset(&schedparam, 0, sizeof(struct sched_param));
	pthread_setschedparam(thread_id, SCHED_NORMAL, &schedparam);

	/* init debug queue */
	init_debug_buffers();

	/* output messages from debug queue */
	while ((!pending_shutdown) && (!debug_done)) {
		usleep(100000);
		output_pending_debug();
	}

	pthread_exit(NULL);
	return NULL;
}
