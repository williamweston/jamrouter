/*****************************************************************************
 *
 * jack.c
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
 *
 *****************************************************************************/
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#ifdef HAVE_JACK_WEAKJACK_H
# include <jack/weakjack.h>
#endif
#include <jack/jack.h>
#include <jack/midiport.h>
#include <glib.h>
#include "jamrouter.h"
#include "timekeeping.h"
#include "jack.h"
#include "jack_midi.h"
#include "midi_event.h"
#include "debug.h"
#include "driver.h"

#ifdef HAVE_JACK_SESSION_H
# include <jack/session.h>
#endif
#ifndef WITHOUT_LASH
# include "lash.h"
#endif


jack_client_t           *jack_audio_client          = NULL;
static char             jack_audio_client_name[64]  = "jamrouter";

jack_port_t             *midi_input_port            = NULL;
jack_port_t             *midi_output_port           = NULL;

JACK_PORT_INFO          *jack_midi_input_ports      = NULL;
JACK_PORT_INFO          *jack_midi_output_ports     = NULL;

int                     jack_running                = 0;
int                     jack_midi_ports_changed     = 0;
int                     jack_rebuilding_port_list   = 0;

char                    *jack_session_uuid          = NULL;

#ifdef HAVE_JACK_SESSION_H
jack_session_event_t    *jack_session_event         = NULL;
#endif

unsigned short          jack_midi_period            = 0;


/*****************************************************************************
 * jack_process_buffer_no_audio()
 *
 * Single jack client for midi
 *****************************************************************************/
int
jack_process_buffer_no_audio(jack_nframes_t nframes, void *UNUSED(arg))
{
	static jack_nframes_t  last_nframes     = 0;
	unsigned short         new_period;

	if (!jack_running || pending_shutdown || (jack_audio_client == NULL)) {
		return 0;
	}

	if (nframes != last_nframes) {
		jack_midi_period = 0;
	}
	last_nframes = nframes;

	new_period = set_midi_cycle_time(jack_midi_period, (int)(nframes));

	jack_process_midi_in(jack_midi_period, (unsigned short)(nframes));

	/* During JAMRouter development, after observing memory reordering issues
	   in the other threads, this was identified as a critical section where
	   memory reordering could potentially do the wrong thing at low buffer
	   sizes in the JACK buffer process callback thread.  With debug disabled
	   (and all the memory fences that come with it), CPU cache reads from the
	   queue could potentially return old values, and writes to the queue
	   could potentially be scheduled late.  Before this memory fence went in,
	   missing already-scheduled events on the JACK input --> MIDI Tx queue at
	   16/96000/tx_latency=1 was encountered often enough to enforce a default
	   tx_latency=2 at 16/96000.  More testing is needed to determine if this
	   memory fence is actually required or even helpful.  At the very least,
	   this memory fence cannot hurt, and serves as an extra safeguard against
	   synchronization errors. */
    asm volatile ("mfence; # read/write fence" : : : "memory");

	jack_process_midi_out(jack_midi_period, (unsigned short)(nframes));

	jack_midi_period = new_period;

	return 0;
}


/*****************************************************************************
 * jack_port_info_free()
 *****************************************************************************/
void
jack_port_info_free(JACK_PORT_INFO *portinfo, int follow)
{
	JACK_PORT_INFO  *cur = portinfo;
	JACK_PORT_INFO  *next;

	while (cur != NULL) {
		if (cur->name != NULL) {
			free(cur->name);
		}
		if (cur->type != NULL) {
			free(cur->type);
		}
		next = cur->next;
		free(cur);
		cur = next;
		if (!follow) {
			break;
		}
	}
}


/*****************************************************************************
 * jack_get_midi_port_list()
 *****************************************************************************/
JACK_PORT_INFO *
jack_get_midi_port_list(long unsigned int flags)
{
	JACK_PORT_INFO  *head = NULL;
	JACK_PORT_INFO  *cur  = NULL;
	JACK_PORT_INFO  *new;
	jack_port_t     *port;
	const char      **port_names;
	int             port_num;

	jack_rebuilding_port_list = 1;

	/* build list of available midi ports */
	port_names = jack_get_ports(jack_audio_client, NULL, NULL, flags);
	for (port_num = 0; port_names[port_num] != NULL; port_num++) {
		port = jack_port_by_name(jack_audio_client, port_names[port_num]);
		if (port && ((long unsigned int)jack_port_flags(port) & flags) &&
		    (strcmp(jack_port_type(port), JACK_DEFAULT_MIDI_TYPE) == 0)) {
			if ((new = malloc(sizeof(JACK_PORT_INFO))) == NULL) {
				jamrouter_shutdown("Out of Memory!\n");
			}
			new->name = strdup(jack_port_name(port));
			new->type = strdup(JACK_DEFAULT_MIDI_TYPE);
			/* rx from port that can output */
			if (flags & JackPortIsOutput) {
				new->connected =
					((midi_input_port != NULL) ?
					 (jack_port_connected_to(midi_input_port,
					                         jack_port_name(port)) ? 1 : 0) : 0);
			}
			/* tx to port than can take input */
			if (flags & JackPortIsInput) {
				new->connected =
					((midi_output_port != NULL) ?
					 (jack_port_connected_to(midi_output_port,
					                         jack_port_name(port)) ? 1 : 0) : 0);
			}
			new->connect_request    = 0;
			new->disconnect_request = 0;
			new->next               = NULL;
			if (head == NULL) {
				head = cur = new;
			}
			else {
				cur->next = new;
				cur = cur->next;
			}
		}
	}
	free(port_names);

	jack_rebuilding_port_list = 0;

	return head;
}


/*****************************************************************************
 * jack_client_registration_handler()
 *
 * Called when a jack client is registered or unregistered.
 * Rebuilds the list of available MIDI ports.
 *****************************************************************************/
void
jack_client_registration_handler(const char *name, int reg, void *UNUSED(arg))
{
	JACK_PORT_INFO  *cur;
	JACK_PORT_INFO  *prev = NULL;
	JACK_PORT_INFO  *head;
	char            client_match[64];

	JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER,
	                "JACK %sregistered client %s.\n",
	                (reg ? "" : "un"),
	                name);

	while (jack_rebuilding_port_list) {
		usleep(1000);
	}

	/* when a client unregisters, we need to manually remove from our list. */
	snprintf(client_match, sizeof(client_match), "%s:", name);
	if (reg == 0) {

		/* rx */
		if (jack_midi_input_ports != NULL) {
			head = cur = jack_midi_input_ports;
			while (cur != NULL) {
				if ((cur->name != NULL) &&
				    (strncmp(cur->name, client_match,
				             strlen(client_match)) == 0)) {
					if (prev == NULL) {
						head = cur->next;
					}
					else {
						prev->next = cur->next;
					}
					jack_port_info_free(cur, 0);
				}
				prev = cur;
				cur = cur->next;
			}
			jack_midi_input_ports = head;
		}

		/* tx */
		if (jack_midi_input_ports != NULL) {
			head = cur = jack_midi_output_ports;
			while (cur != NULL) {
				if ((cur->name != NULL) &&
				    (strncmp(cur->name,
				             client_match, strlen(client_match)) == 0)) {
					if (prev == NULL) {
						head = cur->next;
					}
					else {
						prev->next = cur->next;
					}
					jack_port_info_free(cur, 0);
				}
				prev = cur;
				cur = cur->next;
			}
			jack_midi_output_ports = head;
		}

		jack_midi_ports_changed = 1;
	}
}


/*****************************************************************************
 * jack_port_registration_handler()
 *
 * Called when a jack client registers or unregisters a port.
 * Inserts or deletes port from jamrouter internal midi port list
 * depending on the port's new registration status.
 *
 * TODO:  Make sure list functions are thread-safe!
 *****************************************************************************/
void
jack_port_registration_handler(jack_port_id_t port_id,
                               int reg,
                               void *UNUSED(arg))
{
	JACK_PORT_INFO  *head      = NULL;
	JACK_PORT_INFO  *cur       = NULL;
	JACK_PORT_INFO  *prev      = NULL;
	JACK_PORT_INFO  *new;
	jack_port_t     *port      = jack_port_by_id(jack_audio_client, port_id);
	const char      *port_name = jack_port_name(port);
	int             flags      = jack_port_flags(port);

	JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER, "JACK %sregistered port %d (%s)\n",
	                (reg ? "" : "un"), port_id, port_name);

	while (jack_rebuilding_port_list) {
		usleep(1000);
	}

	/* when a port unregisters, remove it from the list */
	if (reg == 0) {

		/* rx or tx ? */
		if (flags & JackPortIsOutput) {
			head = cur = jack_midi_input_ports;
		}
		else if (flags & JackPortIsInput) {
			head = cur = jack_midi_output_ports;
		}

		/* remove from list */
		while (cur != NULL) {
			if (strcmp(cur->name, port_name) == 0) {
				if (prev == NULL) {
					head = cur->next;
				}
				else {
					prev->next = cur->next;
				}
				jack_port_info_free(cur, 0);
				break;
			}
			prev = cur;
			cur = cur->next;
		}

		/* reset appropriate rx / tx list head */
		if (flags & JackPortIsOutput) {
			jack_midi_input_ports = head;
		}
		else if (flags & JackPortIsInput) {
			jack_midi_output_ports = head;
		}

		jack_midi_ports_changed = 1;
	}

	/* when a port registers, add it to the list */
	else {
		if (!jack_port_is_mine(jack_audio_client, port) &&
		    ((flags & JackPortIsOutput) || (flags & JackPortIsInput)) &&
		    (strcmp(jack_port_type(port), JACK_DEFAULT_MIDI_TYPE) == 0)) {
			if ((new = malloc(sizeof(JACK_PORT_INFO))) == NULL) {
				jamrouter_shutdown("Out of Memory!\n");
			}
			new->name      = strdup(port_name);
			new->type      = strdup(JACK_DEFAULT_MIDI_TYPE);
			if (flags & JackPortIsOutput) {
				head = cur = jack_midi_input_ports;
				new->connected = ((midi_input_port != NULL) ?
				                  (jack_port_connected_to(midi_input_port,
				                                          port_name) ? 1 : 0) : 0);
			}
			else if (flags & JackPortIsInput) {
				head = cur = jack_midi_output_ports;
				new->connected = ((midi_output_port != NULL) ?
				                  (jack_port_connected_to(midi_output_port,
				                                          port_name) ? 1 : 0) : 0);
			}
			new->connect_request    = 0;
			new->disconnect_request = 0;
			new->next               = NULL;
			while (cur != NULL) {
				if (strcmp(cur->name, port_name) > 0) {
					new->next = cur;
					if (prev == NULL) {
						head = new;
					}
					else {
						prev->next = new;
					}
					break;
				}
				prev = cur;
				cur = cur->next;
			}
			if (cur == NULL) {
				if (prev == NULL) {
					head = new;
				}
				else {
					prev->next = new;
				}
			}

			if (flags & JackPortIsOutput) {
				jack_midi_input_ports = head;
			}
			else if (flags & JackPortIsInput) {
				jack_midi_output_ports = head;
			}

			jack_midi_ports_changed = 1;
		}
	}
}


/*****************************************************************************
 * jack_port_connection_handler()
 *
 * Called when a jack client port connects or disconnects.
 * Sets connection status in the jamrouter internal jack port list.
 *****************************************************************************/
void
jack_port_connection_handler(jack_port_id_t a,
                             jack_port_id_t b,
                             int connect,
                             void *UNUSED(arg))
{
	JACK_PORT_INFO  *cur    = NULL;
	jack_port_t     *port_a = jack_port_by_id(jack_audio_client, a);
	jack_port_t     *port_b = jack_port_by_id(jack_audio_client, b);
	int             j       = 0;

	JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER,
	                "JACK port %d (%s) %s port %d (%s)  connect=%d\n",
	                a, jack_port_name(port_a),
	                (connect ? "connected to" : "disconnected from"),
	                b, jack_port_name(port_b),
	                connect);

	while (jack_rebuilding_port_list) {
		usleep(1000);
	}

	if ((strcmp(jack_port_type(port_a), JACK_DEFAULT_MIDI_TYPE) == 0) &&
	    (jack_port_is_mine(jack_audio_client, port_a) ||
	     jack_port_is_mine(jack_audio_client, port_b))) {
		for (cur = jack_midi_input_ports; j < 2; cur = jack_midi_output_ports) {
			while (cur != NULL) {
				if ((strcmp(cur->name, jack_port_name(port_a)) == 0) ||
				    (strcmp(cur->name, jack_port_name(port_b)) == 0)) {
					cur->connected = connect;
				}
				cur = cur->next;
			}
			j++;
		}
	}
}


/*****************************************************************************
 * jack_port_rename_handler()
 *
 * Called when a jack client port is renamed.
 * Resets port's name in the jamrouter internal jack port list.
 *****************************************************************************/
int
jack_port_rename_handler(jack_port_id_t UNUSED(port),
                         const char     *old_name,
                         const char     *new_name,
                         void           *UNUSED(arg))
{
	JACK_PORT_INFO  *cur  = NULL;
	char            *old;
	int             j     = 0;

	while (jack_rebuilding_port_list) {
		usleep(1000);
	}

	for (cur = jack_midi_input_ports; j < 2; cur = jack_midi_output_ports) {
		while (cur != NULL) {
			if (strcmp(cur->name, old_name) == 0) {
				old = cur->name;
				cur->name = strdup(new_name);
				free(old);
			}
			cur = cur->next;
		}
		j++;
	}

	return 0;
}


/*****************************************************************************
 * jack_bufsize_handler()
 *
 * Called when jack sets or changes buffer size.  Timing core detects buffer
 * size changes directly, so currently the handler is only used to maintain
 * a complete implementation of the JACK MIDI API.
 *****************************************************************************/
int
jack_bufsize_handler(jack_nframes_t nframes, void *UNUSED(arg))
{
	/* Make sure buffer doesn't get overrun */
	if ((nframes * DEFAULT_BUFFER_PERIODS) > MAX_BUFFER_SIZE) {
		JAMROUTER_ERROR("JACK requested buffer period size:  %d\n", nframes);
		jamrouter_shutdown("Buffer size exceeded.  Exiting...\n");
	}

	start_midi_clock();

	JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER,
	                "JACK requested buffer size:  %d\n",
	                nframes);

	return 0;
}


/*****************************************************************************
 * jack_samplerate_handler()
 *
 * Called when jack sets or changes sample rate.
 *****************************************************************************/
int
jack_samplerate_handler(jack_nframes_t nframes, void *UNUSED(arg))
{
	JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER,
	                "JACK requested sample rate:  %d\n", nframes);

	/* if jack requests a zero value, just use what we already have */
	if (nframes == 0) {
		return 0;
	}

	/* Changing JACK sample rate midstream not tested */
	if ((sample_rate > 0) && ((unsigned int) sample_rate != nframes)) {
		stop_jack_audio();
	}

	/* First time setting sample rate */
	if (sample_rate == 0) {
		sample_rate    = (int) nframes;

		JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER,
		                "JACK set sample rate:  %d\n", nframes);
	}

	return 0;
}


/*****************************************************************************
 * jack_shutdown_handler()
 *
 * Called when JACK shuts down or closes client.
 *****************************************************************************/
void
jack_shutdown_handler(void *UNUSED(arg))
{
	/* set state so client can be restarted */
	jack_running        = 0;
	jack_thread_p       = 0;
	jack_audio_client   = NULL;
	midi_input_port     = NULL;
	midi_output_port    = NULL;

	JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
	                "JACK shutdown handler called in client thread 0x%lx\n",
	                pthread_self());
}


/*****************************************************************************
 * jack_xrun_handler()
 *
 * Called when jack detects an xrun event.
 *****************************************************************************/
int
jack_xrun_handler(void *UNUSED(arg))
{
	/* This is the best we can do, since an xrun means JACK missed its wakeup
	   time.  The MIDI Tx thread will still deliver what's in the queue on
	   time.  Normal timing will pick up as soon as JACK starts processing its
	   connection graph regularly. */
	JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
	                DEBUG_COLOR_RED "<" DEBUG_COLOR_YELLOW "XRUN"
	                DEBUG_COLOR_RED "> " DEBUG_COLOR_DEFAULT);
	return 0;
}


/*****************************************************************************
 * set_jack_latency()
 *
 * Called when jack sets or changes port latencies.
 *****************************************************************************/
void
set_jack_latency(jack_latency_callback_mode_t mode)
{
	struct timespec         now;
	jack_latency_range_t    range;
	jack_nframes_t          min_adj;
	jack_nframes_t          max_adj;
	jack_nframes_t          byte_frame_len;
	jack_nframes_t          max_jitter;
	unsigned short          period;

	range.min = 0;
	range.max = 0;

	period = get_midi_period(&now);
	switch (sync_info[period].sample_rate) {
	case 22050:
		byte_frame_len = 8;
		max_jitter = 4;
		break;
	case 32000:
		byte_frame_len = 10;
		max_jitter = 5;
		break;
	case 44100:
	case 48000:
		byte_frame_len = 15;
		max_jitter = 6;
		break;
	case 64000:
		byte_frame_len = 20;
		max_jitter = 8;
		break;
	case 88200:
	case 96000:
		byte_frame_len = 30;
		max_jitter = 12;
		break;
	case 176400:
	case 192000:
		byte_frame_len = 60;
		max_jitter = 24;
		break;
	case 384000:
		byte_frame_len = 120;
		max_jitter = 28;
		break;
	default:
		byte_frame_len = sync_info[period].sample_rate * 10 / 31250;
		max_jitter     = sync_info[period].sample_rate / 8000;
	}

	switch (mode) {
	case JackPlaybackLatency:
		min_adj = (jack_nframes_t)((int)(sync_info[period].tx_latency_size +
		                                 sync_info[period].buffer_period_size -
		                                 (unsigned short)(midi_phase_lock)));
		max_adj = min_adj + max_jitter;
		range.min += min_adj;
		range.max += max_adj;
		jack_port_set_latency_range(midi_input_port,
		                            JackPlaybackLatency, &range);
		JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER,
		                "JACK MIDI Input Latency:   min / max  =  %d / %d\n",
		                range.min, range.max);
		break;
	case JackCaptureLatency:
		min_adj = byte_frame_len +
			(jack_nframes_t)((int)(sync_info[period].rx_latency_size +
			                       (unsigned short)(midi_phase_lock)));
		max_adj = min_adj + max_jitter;
		range.min += min_adj;
		range.max += max_adj;
		jack_port_set_latency_range(midi_output_port,
		                            JackCaptureLatency, &range);
		JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER,
		                "JACK MIDI Output Latency:  min / max  =  %d / %d\n",
		                range.min, range.max);
		break;
	}
}


/*****************************************************************************
 * jack_latency_handler()
 *
 * Called when jack sets or changes port latencies.
 *****************************************************************************/
#ifdef ENABLE_JACK_LATENCY_CALLBACK
void
jack_latency_handler(jack_latency_callback_mode_t mode, void *UNUSED(arg))
{
	set_jack_latency(mode);
}
#endif /* ENABLE_JACK_LATENCY_CALLBACK */


/*****************************************************************************
 * jack_graph_order_handler()
 *
 * Called when jack sets or changes the graph order.  Rebuilds the list of
 * available MIDI ports.
 *****************************************************************************/
#ifdef ENABLE_JACK_GRAPH_ORDER_CALLBACK
int
jack_graph_order_handler(void *arg)
{
	JACK_PORT_INFO  *old_midi_ports = jack_midi_ports;
	JACK_PORT_INFO  *new_midi_ports;

	JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER, "JACK graph order changed!\n");

	return 0;
}
#endif /* ENABLE_JACK_GRAPH_ORDER_CALLBACK */


/*****************************************************************************
 * jack_session_handler()
 *
 * Called when jack needs to save the session.
 *****************************************************************************/
#ifdef HAVE_JACK_SESSION_H
void
jack_session_handler(jack_session_event_t *event, void *UNUSED(arg))
{
	char                    cmd[256];
	size_t                  len;

	len = strlen(jamrouter_full_cmdline);

	if (jamrouter_full_cmdline[len - 1] == '"') {
		jamrouter_full_cmdline[len - 1] = '\0';
		snprintf(cmd, sizeof(cmd), "%s -u %s\"",
		         jamrouter_full_cmdline, event->client_uuid);
	}
	else {
		snprintf(cmd, sizeof(cmd), "%s -u %s",
		         jamrouter_full_cmdline, event->client_uuid);
	}
	event->command_line = strdup(cmd);
	jack_session_reply(jack_audio_client, event);

	/* keep session event and let watchdog do the real work */
	jack_session_event = event;
}
#endif /* HAVE_JACK_SESSION_H */


/*****************************************************************************
 * jack_error_handler()
 *****************************************************************************/
void
jack_error_handler(const char *msg)
{
	JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER, "JACK Error:  %s\n", msg);
}


/*****************************************************************************
 * jack_audio_init()
 *
 * Initialize the jack client, register ports, and set callbacks.  All that
 * needs to be done before time to call jack_activate() should be done here.
 *****************************************************************************/
int
jack_audio_init(int scan)
{
	jack_options_t  options             = JackNoStartServer | JackUseExactName;
	jack_status_t   client_status;
	unsigned int    new_sample_rate;
	unsigned int    new_buffer_period_size;

	JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
	                "Initializing JACK client from thread 0x%lx\n",
	                pthread_self());

	jack_set_error_function(jack_error_handler);

	if (jack_audio_client != NULL) {
		JAMROUTER_WARN("Warning: closing stale JACK client...\n");
		jack_client_close(jack_audio_client);
		jack_audio_client = NULL;
	}

	/* open a client connection to the JACK server */
	snprintf(jack_audio_client_name,
	         sizeof(jack_audio_client_name) - 1,
	         "jamrouter%c",
	         jamrouter_instance > 1 ? ('0' + jamrouter_instance) : '\0');
	JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
	             "Opening JACK client '%s'...\n",
	             jack_audio_client_name);
	jack_audio_client = jack_client_open(jack_audio_client_name,
	                                     options,
	                                     &client_status);

	/* deal with non-unique client name */
	if (client_status & JackNameNotUnique) {
		JAMROUTER_ERROR("Unable to start JACK client '%s'!\n",
		                jack_audio_client_name);
		return 1;
	}

	/* deal with jack server problems */
	if (client_status & (JackServerFailed | JackServerError)) {
		JAMROUTER_ERROR("Unable to connect to JACK server.  Status = 0x%2.0x\n",
		                client_status);
		return 1;
	}

	/* deal with missing client */
	if (jack_audio_client == NULL) {
		JAMROUTER_ERROR("Unable to open JACK client.  Status = 0x%2.0x\n",
		                client_status);
		return 1;
	}

	/* build lists of available midi ports */

	/*rx */
	if (jack_midi_input_ports != NULL) {
		jack_port_info_free(jack_midi_input_ports, 1);
	}
	jack_midi_input_ports = jack_get_midi_port_list(JackPortIsOutput);

	/* tx */
	if (jack_midi_output_ports != NULL) {
		jack_port_info_free(jack_midi_output_ports, 1);
	}
	jack_midi_output_ports = jack_get_midi_port_list(JackPortIsInput);

	if (scan) {
		return 0;
	}

	/* callback for jack shutting down, needs to be set as early as possible. */
	jack_on_shutdown(jack_audio_client, jack_shutdown_handler, 0);

	JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
	                "Unique JACK client name '%s' assigned.\n",
	                jack_audio_client_name);

	/* notify once if we started jack server */
	if (client_status & JackServerStarted) {
		JAMROUTER_DEBUG(DEBUG_CLASS_INIT, "JACK server started.\n");
	}

	/* report realtime scheduling in JACK */
	if (debug) {
		if (jack_is_realtime(jack_audio_client)) {
			JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
			                "JACK is running with realtime scheduling.\n");
		}
		else {
			JAMROUTER_WARN("WARNING:  "
			               "JACK is running without realtime scheduling.\n");
		}
	}

	/* get sample rate from jack */
	new_sample_rate = jack_get_sample_rate(jack_audio_client);

	JAMROUTER_DEBUG(DEBUG_CLASS_INIT, "JACK sample rate: %d\n", new_sample_rate);

	/* keep track of sample rate changes */
	if (sample_rate != (int)(new_sample_rate)) {
		sample_rate = (int)(new_sample_rate);
	}

	JAMROUTER_DEBUG(DEBUG_CLASS_INIT, "Internal sample rate: %d\n", sample_rate);

	/* callback for setting our sample rate when jack tells us to */
	jack_set_sample_rate_callback(jack_audio_client, jack_samplerate_handler, 0);

	/* get buffer size */
	new_buffer_period_size = jack_get_buffer_size(jack_audio_client);
	if (new_buffer_period_size > MAX_BUFFER_SIZE) {
		JAMROUTER_WARN("JACK requested buffer size:  %d\n",
		               new_buffer_period_size);
		JAMROUTER_ERROR("JACK buffer size exceeded.  Closing client...\n");
		jack_client_close(jack_audio_client);
		jack_audio_client  = NULL;
		jack_running = 0;
		return 1;
	}

	JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
	                "JACK audio buffer size:  %d\n",
	                new_buffer_period_size);

	init_sync_info((unsigned int)(new_sample_rate),
	               (short unsigned int)(new_buffer_period_size));

	/* register midi input/output ports */
	midi_input_port = jack_port_register(jack_audio_client, "midi_in",
	                                     JACK_DEFAULT_MIDI_TYPE,
	                                     JackPortIsInput, 0);
	midi_output_port = jack_port_register(jack_audio_client, "midi_out",
	                                      JACK_DEFAULT_MIDI_TYPE,
	                                      JackPortIsOutput, 0);

	/* set all callbacks needed for jack */
	jack_set_process_callback
		(jack_audio_client, jack_process_buffer_no_audio, (void *) NULL);
	jack_set_buffer_size_callback
		(jack_audio_client, jack_bufsize_handler, 0);
	jack_set_xrun_callback
		(jack_audio_client, jack_xrun_handler, 0);
	jack_set_client_registration_callback
		(jack_audio_client, jack_client_registration_handler, (void *) NULL);
	jack_set_port_registration_callback
		(jack_audio_client, jack_port_registration_handler, (void *) NULL);
	jack_set_port_connect_callback
		(jack_audio_client, jack_port_connection_handler, (void *) NULL);
#ifdef HAVE_JACK_SET_PORT_RENAME_CALLBACK
	jack_set_port_rename_callback
		(jack_audio_client, jack_port_rename_handler, (void *) NULL);
#endif /* HAVE_JACK_SET_PORT_RENAME_CALLBACK */
#ifdef HAVE_JACK_SET_SESSION_CALLBACK
	if (jack_set_session_callback) {
		jack_set_session_callback
			(jack_audio_client, jack_session_handler, (void *) NULL);
	}
#endif /* HAVE_JACK_SET_SESSION_CALLBACK */
#ifdef ENABLE_JACK_GRAPH_ORDER_CALLBACK
	jack_set_graph_order_callback(jack_audio_client,
	                              jack_graph_order_handler,
	                              (void *) NULL);
#endif /* ENABLE_JACK_GRAPH_ORDER_CALLBACK */
#if defined(HAVE_JACK_SET_LATENCY_CALLBACK) && defined(ENABLE_JACK_LATENCY_CALLBACK)
	if (jack_set_latency_callback) {
		jack_set_latency_callback
			(jack_audio_client, jack_latency_handler, (void *) NULL);
	}
#endif /* HAVE_JACK_SET_LATENCY_CALLBACK && ENABLE_JACK_LATENCY_CALLBACK */

#ifndef WITHOUT_LASH
	if (!lash_disabled) {
		lash_client_set_jack_name(jack_audio_client);
	}
#endif

	return 0;
}


/*****************************************************************************
 * jack_start()
 *
 * Start jack client and attach to playback ports.
 *****************************************************************************/
int
jack_start(void)
{
	JACK_PORT_INFO      *cur;
	const char          *portname;
	char                thread_name[16];

	/* activate client (callbacks start, so everything needs to be ready) */
	if (jack_activate(jack_audio_client)) {
		JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
		                "Unable to activate JACK client.\n");
		jack_client_close(jack_audio_client);
		jack_running        = 0;
		jack_thread_p       = 0;
		jack_audio_client   = NULL;
		midi_input_port     = NULL;
		return 1;
	}

	/* all up and running now */
	jack_running = 1;
	jack_thread_p = jack_client_thread_id(jack_audio_client);

	/* set realtime scheduling and priority */
	snprintf(thread_name, 16, "jamrouter%c-jack", ('0' + jamrouter_instance));
	pthread_setname_np(jack_thread_p, thread_name);

	/* broadcast the audio ready condition */
	pthread_mutex_lock(&jack_audio_ready_mutex);
	jack_audio_ready = 1;
	pthread_cond_broadcast(&jack_audio_ready_cond);
	pthread_mutex_unlock(&jack_audio_ready_mutex);

	/* connect ports.  in/out is from server perspective */
	cur = jack_midi_input_ports;
	while ((cur != NULL) && (jack_input_port_name != NULL)) {
		JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
		                "Checking JACK MIDI Input port '%s'\n", cur->name);
		if (strcmp(jack_input_port_name, cur->name) == 0) {
			portname = jack_port_name(midi_input_port);
			if (jack_connect(jack_audio_client, cur->name, portname)) {
				JAMROUTER_WARN("Unable to connect '%s' --> '%s'\n",
				               cur->name, portname);
			}
			else {
				break;
			}
		}
		cur = cur->next;
	}
	cur = jack_midi_output_ports;
	while ((cur != NULL) && (jack_output_port_name != NULL)) {
		JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
		                "Checking JACK MIDI Output port '%s'\n", cur->name);
		if (strcmp(jack_output_port_name, cur->name) == 0) {
			portname = jack_port_name(midi_output_port);
			if (jack_connect(jack_audio_client, portname, cur->name)) {
				JAMROUTER_WARN("Unable to connect '%s' --> '%s'\n",
				               portname, cur->name);
			}
		}
		cur = cur->next;
	}

	return 0;
}


/*****************************************************************************
 * jack_stop()
 *
 * Closes the JACK client and cleans up internal state.
 * Unless a shutdown condition has been specified, this will cause the
 * watchdog loop to restart it.  To be called from other threads.
 *****************************************************************************/
int
jack_stop(void)
{
	jack_client_t   *tmp_client;

	if ((jack_audio_client != NULL) && jack_running && (jack_thread_p) != 0) {
		tmp_client          = jack_audio_client;
		jack_audio_client   = NULL;
		jack_thread_p       = 0;
		midi_input_port     = NULL;
		midi_output_port    = NULL;

#ifdef JACK_DEACTIVATE_BEFORE_CLOSE
		jack_deactivate(tmp_client);
#endif
		jack_client_close(tmp_client);
	}

	jack_running = 0;
	jack_audio_stopped = 1;

	JAMROUTER_DEBUG(DEBUG_CLASS_INIT, "JACK stopped.  Client closed.\n");

	return 0;
}


/*****************************************************************************
 * jack_restart()
 *
 * Calls jack_stop() and hopes the audio watchdog will restart jack.
 *****************************************************************************/
void
jack_restart(void)
{
	jack_stop();
}


/*****************************************************************************
 * jack_watchdog_cycle()
 *
 * Called by the audio watchdog to handle (dis)connect requests.
 *****************************************************************************/
void
jack_watchdog_cycle(void)
{
	JACK_PORT_INFO  *cur;
	int             save_and_quit = 0;
	int             j             = 0;

	if ((jack_audio_client == NULL) || !jack_running || (jack_thread_p == 0)) {
		jack_running = 0;
		jack_audio_stopped = 1;
		JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER | DEBUG_CLASS_INIT,
		                "JACK Watchdog:  JACK quit running.\n");
	}
#ifdef HAVE_JACK_SESSION_H
	if (jack_session_event != NULL) {
		switch(jack_session_event->type) {
		case JackSessionSaveAndQuit:
			save_and_quit = 1;
		case JackSessionSave:
		case JackSessionSaveTemplate:
			jack_session_event_free(jack_session_event);
			jack_session_event = NULL;
		}
		if (save_and_quit) {
			jamrouter_shutdown("Saved JACK Session.  Goodbye!\n");
		}
	}
#endif /* HAVE_JACK_SESSION_H */
	for (cur = jack_midi_input_ports; j < 2; cur = jack_midi_output_ports) {
		while (cur != NULL) {
			if (cur->connect_request) {
				jack_connect(jack_audio_client, cur->name, jack_port_name(midi_input_port));
				if (jack_port_connected_to(midi_input_port, cur->name)) {
					cur->connected          = 1;
					cur->connect_request    = 0;
					cur->disconnect_request = 0;
				}
				else {
					cur->connected          = 0;
					cur->connect_request    = 0;
					cur->disconnect_request = 0;
				}
			}
			else if (cur->disconnect_request) {
				jack_disconnect(jack_audio_client, cur->name, jack_port_name(midi_input_port));
				if (jack_port_connected_to(midi_input_port, cur->name)) {
					cur->connected          = 1;
					cur->connect_request    = 0;
					cur->disconnect_request = 1;
				}
				else {
					cur->connected = 0;
					cur->connect_request    = 0;
					cur->disconnect_request = 0;
				}
			}
			cur = cur->next;
		}
		j++;
	}
}
