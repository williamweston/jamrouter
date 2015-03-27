/*****************************************************************************
 *
 * driver.c
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
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include "jamrouter.h"
#include "driver.h"
#include "timekeeping.h"
#include "debug.h"
#include "rawmidi.h"
#include "alsa_seq.h"
#include "jack.h"

#ifndef WITHOUT_LASH
# include "lash.h"
#endif


char                audio_driver_status_msg[256];

char                *audio_driver_name          = "jack";
char                *midi_driver_name           = "dummy";

int                 midi_driver                 = MIDI_DRIVER_NONE;


DRIVER_INT_FUNC     audio_init_func;
DRIVER_FUNC         audio_start_func;
DRIVER_FUNC         audio_stop_func;
DRIVER_VOID_FUNC    audio_restart_func;
THREAD_FUNC         audio_thread_func;
DRIVER_VOID_FUNC    audio_watchdog_func;

DRIVER_FUNC         midi_init_func;
DRIVER_FUNC         midi_start_func;
DRIVER_FUNC         midi_stop_func;
DRIVER_VOID_FUNC    midi_restart_func;
THREAD_FUNC         midi_rx_thread_func;
THREAD_FUNC         midi_tx_thread_func;
DRIVER_VOID_FUNC    midi_watchdog_func;

pthread_mutex_t     jack_audio_ready_mutex;
pthread_cond_t      jack_audio_ready_cond       = PTHREAD_COND_INITIALIZER;
int                 jack_audio_ready            = 0;
int                 jack_audio_stopped          = 0;

pthread_mutex_t     midi_rx_ready_mutex;
pthread_cond_t      midi_rx_ready_cond          = PTHREAD_COND_INITIALIZER;
int                 midi_rx_ready               = 0;
int                 midi_rx_stopped             = 0;

pthread_mutex_t     midi_tx_ready_mutex;
pthread_cond_t      midi_tx_ready_cond          = PTHREAD_COND_INITIALIZER;
int                 midi_tx_ready               = 0;
int                 midi_tx_stopped             = 0;


char *midi_driver_names[] = {
	"dummy",
	"jack",
	"seq",
	"raw",
#ifdef ENABLE_RAWMIDI_GENERIC
	"generic",
#endif
#ifdef ENABLE_RAWMIDI_OSS
	"oss",
#endif
#ifdef ENABLE_RAWMIDI_OSS2
	"oss2",
#endif
	NULL
};


/*****************************************************************************
 * select_midi_driver()
 *****************************************************************************/
void
select_midi_driver(char *driver_name, int driver_id)
{
	if (driver_name == NULL) {
		driver_name = "";
	}
	if ((driver_id == MIDI_DRIVER_JACK) ||
	    (strcmp(driver_name, "jack") == 0)) {
		midi_driver_name    = "jack";
		midi_driver         = MIDI_DRIVER_JACK;
		midi_init_func      = NULL;
		midi_start_func     = NULL;
		midi_stop_func      = NULL;
		midi_restart_func   = NULL;
		midi_rx_thread_func = NULL;
		midi_tx_thread_func = NULL;
		midi_watchdog_func  = NULL;
	}
	else if ((driver_id == MIDI_DRIVER_ALSA_SEQ) ||
	         (strcmp(driver_name, "alsa") == 0) ||
	         (strcmp(driver_name, "seq") == 0) ||
	         (strcmp(driver_name, "alsa-seq") == 0)) {
		midi_driver_name    = "alsa-seq";
		midi_driver         = MIDI_DRIVER_ALSA_SEQ;
		midi_init_func      = &alsa_seq_init;
		midi_start_func     = NULL;
		midi_stop_func      = NULL;
		midi_restart_func   = NULL;
		midi_rx_thread_func = &alsa_seq_rx_thread;
		midi_tx_thread_func = &alsa_seq_tx_thread;
		midi_watchdog_func  = &alsa_seq_watchdog_cycle;
	}
	else if ((driver_id == MIDI_DRIVER_RAW_ALSA) ||
	         (strcmp(driver_name, "raw") == 0) ||
	         (strcmp(driver_name, "rawmidi") == 0) ||
	         (strcmp(driver_name, "raw-alsa") == 0) ||
	         (strcmp(driver_name, "alsa-raw") == 0)) {
		midi_driver_name    = "alsa-raw";
		midi_driver         = MIDI_DRIVER_RAW_ALSA;
		midi_init_func      = &rawmidi_init;
		midi_start_func     = NULL;
		midi_stop_func      = NULL;
		midi_restart_func   = NULL;
		midi_rx_thread_func = &raw_midi_rx_thread;
		midi_tx_thread_func = &raw_midi_tx_thread;
		midi_watchdog_func  = &rawmidi_watchdog_cycle;
	}
#ifdef ENABLE_RAWMIDI_GENERIC
	else if ((driver_id == MIDI_DRIVER_RAW_GENERIC) ||
	         (strcmp(driver_name, "generic") == 0)) {
		midi_driver_name    = "generic";
		midi_driver         = MIDI_DRIVER_RAW_GENERIC;
		midi_init_func      = &rawmidi_init;
		midi_start_func     = NULL;
		midi_stop_func      = NULL;
		midi_restart_func   = NULL;
		midi_rx_thread_func = &raw_midi_rx_thread;
		midi_tx_thread_func = &raw_midi_tx_thread;
		midi_watchdog_func  = NULL;
	}
#endif
#ifdef ENABLE_RAWMIDI_OSS
	else if ((driver_id == MIDI_DRIVER_RAW_OSS) ||
	         (strcmp(driver_name, "oss") == 0)) {
		midi_driver_name    = "oss";
		midi_driver         = MIDI_DRIVER_RAW_OSS;
		midi_init_func      = &rawmidi_init;
		midi_start_func     = NULL;
		midi_stop_func      = NULL;
		midi_restart_func   = NULL;
		midi_rx_thread_func = &raw_midi_rx_thread;
		midi_tx_thread_func = &raw_midi_tx_thread;
		midi_watchdog_func  = NULL;
	}
#endif
#ifdef ENABLE_RAWMIDI_OSS2
	else if ((driver_id == MIDI_DRIVER_RAW_OSS2) ||
	         (strcmp(driver_name, "oss2") == 0)) {
		midi_driver_name    = "oss2";
		midi_driver         = MIDI_DRIVER_RAW_OSS2;
		midi_init_func      = &rawmidi_init;
		midi_start_func     = NULL;
		midi_stop_func      = NULL;
		midi_restart_func   = NULL;
		midi_rx_thread_func = &raw_midi_rx_thread;
		midi_tx_thread_func = &raw_midi_tx_thread;
		midi_watchdog_func  = NULL;
	}
#endif
	else if ((driver_id == MIDI_DRIVER_NONE) ||
	         (strcmp(driver_name, "none") == 0) ||
	         (strcmp(driver_name, "dummy") == 0)) {
		midi_driver_name    = "dummy";
		midi_driver         = MIDI_DRIVER_NONE;
		midi_init_func      = NULL;
		midi_start_func     = NULL;
		midi_stop_func      = NULL;
		midi_restart_func   = NULL;
		midi_rx_thread_func = NULL;
		midi_tx_thread_func = NULL;
		midi_watchdog_func  = NULL;
	}
}


/*****************************************************************************
 * init_jack_audio_driver()
 *****************************************************************************/
void
init_jack_audio_driver(void)
{
	audio_driver_name     = "jack";
	audio_init_func       = &jack_audio_init;
	audio_start_func      = &jack_start;
	audio_stop_func       = &jack_stop;
	audio_restart_func    = &jack_restart;
	audio_thread_func     = NULL;
	audio_watchdog_func   = &jack_watchdog_cycle;
}


/*****************************************************************************
 * init_jack_audio()
 *****************************************************************************/
void
init_jack_audio(void)
{
	int     j;

	/* connect to jack server, retrying for up to 5 seconds */
	for (j = 0; j < 5; j++) {
		if (jack_audio_init(0) == 0) {
			if (sample_rate != 0) {
				break;
			}
		}
		else {
			JAMROUTER_WARN("Waiting for JACK server to start...\n");
			sleep(1);
		}
	}
	/* give up if jack server was not found in 5 seconds */
	if (j == 5) {
		jamrouter_shutdown("Unable to conect to JACK server.  Is JACK running?\n");
	}
}


/*****************************************************************************
 * start_jack_audio()
 *****************************************************************************/
void
start_jack_audio(void)
{
	/* ready for jack to start running our process callback */
	if (jack_start() == 0) {
		JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER,
		                "Main: Started JACK with client threads:  0x%lx\n",
		                jack_thread_p);
	}
	else {
	    jamrouter_shutdown("Unable to start JACK client.\n");
	}
}

/*****************************************************************************
 * wait_jack_audio_start()
 *****************************************************************************/
void
wait_jack_audio_start(void)
{
		pthread_mutex_lock(&jack_audio_ready_mutex);
		if (!jack_audio_ready) {
			pthread_cond_wait(&jack_audio_ready_cond, &jack_audio_ready_mutex);
		}
		pthread_mutex_unlock(&jack_audio_ready_mutex);
}


/*****************************************************************************
 * stop_jack_audio()
 *****************************************************************************/
void
stop_jack_audio(void)
{
	jack_audio_stopped  = 1;
	jack_stop();
}


/*****************************************************************************
 * wait_jack_audio_stop()
 *****************************************************************************/
void
wait_jack_audio_stop(void)
{
	while (jack_thread_p != 0) {
		usleep(125000);
	}
}


/*****************************************************************************
 * init_midi()
 *****************************************************************************/
void
init_midi(void)
{
	if (midi_init_func != NULL) {
		if (midi_init_func() != 0) {
			JAMROUTER_WARN("Unable to initialize MIDI input driver '%s'.\n",
			            midi_driver_name);
			jamrouter_shutdown("");
		}
	}
}


/*****************************************************************************
 * start_midi()
 *****************************************************************************/
void
start_midi(void)
{
	start_midi_rx();
	start_midi_tx();
}


/*****************************************************************************
 * start_midi_rx()
 *****************************************************************************/
void
start_midi_rx(void)
{
#ifdef ENABLE_DEBUG
	int     saved_errno;
#endif
	int     ret;

	if (midi_rx_thread_func != NULL) {
		init_rt_mutex(&midi_rx_ready_mutex, 1);
		if ((ret = pthread_create(&midi_rx_thread_p, NULL,
		                          midi_rx_thread_func, NULL)) != 0) {
#ifdef ENABLE_DEBUG
			saved_errno = errno;
			JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
			                "Unable to start MIDI Rx thread:  "
			                "error %d (%s).\n  errno=%d (%s)\n",
			                ret,
			                (ret == EAGAIN) ? "EAGAIN" :
			                (ret == EINVAL) ? "EINVAL" :
			                (ret == EPERM)  ? "EPERM"  : "",
			                saved_errno,
			                strerror(saved_errno));
#endif
			jamrouter_shutdown("Shutting Down.");
		}
	}
}


/*****************************************************************************
 * start_midi_tx()
 *****************************************************************************/
void
start_midi_tx(void)
{
#ifdef ENABLE_DEBUG
	int     saved_errno;
#endif
	int     ret;

	if (midi_tx_thread_func != NULL) {
		init_rt_mutex(&midi_tx_ready_mutex, 1);
		if ((ret = pthread_create(&midi_tx_thread_p, NULL,
		                          midi_tx_thread_func, NULL)) != 0) {
#ifdef ENABLE_DEBUG
			saved_errno = errno;
			JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
			                "Unable to start MIDI Tx thread:  "
			                "error %d (%s).\n  errno=%d (%s)\n",
			                ret,
			                (ret == EAGAIN) ? "EAGAIN" :
			                (ret == EINVAL) ? "EINVAL" :
			                (ret == EPERM)  ? "EPERM"  : "",
			                saved_errno,
			                strerror(saved_errno));
#endif
			jamrouter_shutdown("Shutting Down.");
		}
	}
}


/*****************************************************************************
 * wait_midi_rx_start()
 *****************************************************************************/
void
wait_midi_rx_start(void)
{
	if (midi_rx_thread_func != NULL) {
		pthread_mutex_lock(&midi_rx_ready_mutex);
		if (!midi_rx_ready) {
			pthread_cond_wait(&midi_rx_ready_cond, &midi_rx_ready_mutex);
		}
		pthread_mutex_unlock(&midi_rx_ready_mutex);
	}
}


/*****************************************************************************
 * wait_midi_tx_start()
 *****************************************************************************/
void
wait_midi_tx_start(void)
{
	if (midi_tx_thread_func != NULL) {
		pthread_mutex_lock(&midi_tx_ready_mutex);
		if (!midi_tx_ready) {
			pthread_cond_wait(&midi_tx_ready_cond, &midi_tx_ready_mutex);
		}
		pthread_mutex_unlock(&midi_tx_ready_mutex);
	}
}


/*****************************************************************************
 * stop_midi_rx()
 *****************************************************************************/
void
stop_midi_rx(void)
{
	midi_rx_stopped = 1;
}


/*****************************************************************************
 * stop_midi_tx()
 *****************************************************************************/
void
stop_midi_tx(void)
{
	midi_tx_stopped = 1;
}


/*****************************************************************************
 * wait_midi_rx_stop()
 *****************************************************************************/
void
wait_midi_rx_stop(void)
{
	if (midi_rx_thread_p != 0) {
		pthread_join(midi_rx_thread_p,  NULL);
		usleep(125000);
	}
}


/*****************************************************************************
 * wait_midi_tx_stop()
 *****************************************************************************/
void
wait_midi_tx_stop(void)
{
	if (midi_tx_thread_p != 0) {
		pthread_join(midi_tx_thread_p,  NULL);
		usleep(125000);
	}
}


/*****************************************************************************
 * restart_midi()
 *****************************************************************************/
void
restart_midi(void)
{
	stop_midi_rx();
	stop_midi_tx();
	wait_midi_rx_stop();
	wait_midi_tx_stop();
	init_midi();
	start_midi_rx();
	start_midi_tx();
	wait_midi_rx_start();
	wait_midi_tx_start();
}


/*****************************************************************************
 * jamrouter_watchdog()
 *****************************************************************************/
void
jamrouter_watchdog(void)
{
	while (!pending_shutdown) {
		output_pending_debug();

		if (audio_watchdog_func != NULL) {
			audio_watchdog_func();
			output_pending_debug();
		}
		if (midi_watchdog_func != NULL) {
			midi_watchdog_func();
			output_pending_debug();
		}

#ifndef WITHOUT_LASH
		if (!lash_disabled && !pending_shutdown) {
			lash_poll_event();
			output_pending_debug();
		}
#endif
		if (midi_tx_stopped && !pending_shutdown) {
			wait_midi_tx_stop();
			output_pending_debug();
		}
		if (midi_rx_stopped && !pending_shutdown) {
			wait_midi_rx_stop();
			output_pending_debug();
		}
		if (jack_audio_stopped && !pending_shutdown) {
			wait_jack_audio_stop();
			init_jack_audio();
			output_pending_debug();
		}
		if (midi_tx_stopped && !pending_shutdown) {
			midi_tx_stopped = 0;
			init_midi();
			start_midi_tx();
			output_pending_debug();
		}
		if (jack_audio_stopped && !pending_shutdown) {
			jack_audio_stopped = 0;
			start_jack_audio();
			output_pending_debug();
			wait_jack_audio_start();
			query_audio_driver_status(audio_driver_status_msg);
			JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER, "%s", audio_driver_status_msg);
			output_pending_debug();
		}
		if (midi_rx_stopped && !pending_shutdown) {
			midi_rx_stopped = 0;
			start_midi_rx();
			output_pending_debug();
			wait_midi_rx_start();
		}

		usleep(33333);
	}
}


/*****************************************************************************
 * scan_midi()
 *****************************************************************************/
void
scan_midi(void)
{
	ALSA_SEQ_INFO           *alsa_seq_info;
	ALSA_SEQ_PORT           *seq_port_list;
	ALSA_RAWMIDI_HW_INFO    *rawmidi_hw_list;
	JACK_PORT_INFO          *cur;

	jamrouter_instance = get_instance_num();
	printf("Scanning MIDI devices and ports....\n");

	/* scan for ALSA Seq capture and playback devices */
	if ((alsa_seq_info = malloc(sizeof(ALSA_SEQ_INFO))) == NULL) {
		jamrouter_shutdown("Out of memory!\n");
	}
	memset(alsa_seq_info, 0, sizeof(ALSA_SEQ_INFO));
	alsa_seq_info->seq = NULL;

	/* open the sequencer */
	if (snd_seq_open(& (alsa_seq_info->seq), "default",
	                 SND_SEQ_OPEN_INPUT | SND_SEQ_OPEN_OUTPUT, SND_SEQ_NONBLOCK) < 0) {
		printf("Unable to open ALSA sequencer.\n");
		return;
	}

	/* get list of all capture ports  */
	if ((alsa_seq_info->capture_ports =
	     alsa_seq_get_port_list(alsa_seq_info,
	                            (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ),
	                            alsa_seq_info->capture_ports)) == NULL) {
		printf("Unable to get ALSA sequencer port list.\n");
	}
	seq_port_list = alsa_seq_info->capture_ports;
	if (seq_port_list != NULL) {
		printf("\nFound ALSA sequencer capture ports:\n");
	}
	while (seq_port_list != NULL) {
		printf("    %s\t%s: %s\n",
		       seq_port_list->alsa_name,
		       seq_port_list->client_name,
		       seq_port_list->port_name);
		seq_port_list = seq_port_list->next;
	}
	alsa_seq_port_free(alsa_seq_info->capture_ports);

	/* get list of all playback ports  */
	if ((alsa_seq_info->playback_ports =
	     alsa_seq_get_port_list(alsa_seq_info,
	                            (SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE),
	                            alsa_seq_info->playback_ports)) == NULL) {
		printf("Unable to get ALSA sequencer playback port list.\n");
	}
	seq_port_list = alsa_seq_info->playback_ports;
	if (seq_port_list != NULL) {
		printf("\nFound ALSA sequencer playback ports:\n");
	}
	while (seq_port_list != NULL) {
		printf("    %s\t%s: %s\n",
		       seq_port_list->alsa_name,
		       seq_port_list->client_name,
		       seq_port_list->port_name);
		seq_port_list = seq_port_list->next;
	}
	alsa_seq_port_free(alsa_seq_info->playback_ports);

	snd_seq_close(alsa_seq_info->seq);
	free(alsa_seq_info);

#ifdef ENABLE_RAWMIDI_ALSA_RAW

	/* ALSA Raw MIDI rx */
	if (alsa_rawmidi_rx_hw != NULL) {
		alsa_rawmidi_hw_info_free(alsa_rawmidi_rx_hw);
	}
	alsa_rawmidi_rx_hw = alsa_rawmidi_get_hw_list(SND_RAWMIDI_STREAM_INPUT);
	if (alsa_rawmidi_rx_hw != NULL) {
		printf("\nFound ALSA Raw MIDI Rx hardware devices:\n");
		rawmidi_hw_list = alsa_rawmidi_rx_hw;
		while (rawmidi_hw_list != NULL) {
			printf("    %s\t%s: %s: %s\n",
			       rawmidi_hw_list->alsa_name,
			       rawmidi_hw_list->device_id,
			       rawmidi_hw_list->device_name,
			       rawmidi_hw_list->subdevice_name);
			rawmidi_hw_list = rawmidi_hw_list->next;
		}
	}

	/* ALSA Raw MIDI tx */
	if (alsa_rawmidi_tx_hw != NULL) {
		alsa_rawmidi_hw_info_free(alsa_rawmidi_tx_hw);
	}
	alsa_rawmidi_tx_hw = alsa_rawmidi_get_hw_list(SND_RAWMIDI_STREAM_OUTPUT);
	if (alsa_rawmidi_tx_hw != NULL) {
		printf("\nFound ALSA Raw MIDI Tx hardware devices:\n");
		rawmidi_hw_list = alsa_rawmidi_tx_hw;
		while (rawmidi_hw_list != NULL) {
			printf("    %s\t%s: %s: %s\n",
			       rawmidi_hw_list->alsa_name,
			       rawmidi_hw_list->device_id,
			       rawmidi_hw_list->device_name,
			       rawmidi_hw_list->subdevice_name);
			rawmidi_hw_list = rawmidi_hw_list->next;
		}
	}
#endif

	/* initialize JACK audio system based on selected driver */
	JAMROUTER_DEBUG(DEBUG_CLASS_INIT, "Initializing JACK...\n");
	jack_audio_init(1);

	/* JACK rx */
	if (jack_midi_input_ports == NULL) {
		jack_midi_input_ports = jack_get_midi_port_list(JackPortIsOutput);
	}
	if ((jack_midi_input_ports != NULL)) {
		printf("\nFound JACK MIDI Input Ports:\n");
		cur = jack_midi_input_ports;
		while (cur != NULL) {
			printf("    %s\n", cur->name);
			cur = cur->next;
		}
	}

	/* JACK tx */
	if (jack_midi_output_ports == NULL) {
		jack_midi_output_ports = jack_get_midi_port_list(JackPortIsInput);
	}
	if ((jack_midi_output_ports != NULL)) {
		printf("\nFound JACK MIDI Output Ports:\n");
		cur = jack_midi_output_ports;
		while (cur != NULL) {
			printf("    %s\n", cur->name);
			cur = cur->next;
		}
	}

	printf("\n");
}


/*****************************************************************************
 * audio_driver_running()
 *****************************************************************************/
int
audio_driver_running(void)
{
	return jack_audio_ready;
}


/*****************************************************************************
 * query_audio_driver_status()
 *****************************************************************************/
void
query_audio_driver_status(char *buf)
{
	TIMESTAMP       now;
	unsigned short  period = get_midi_period(&now);

	snprintf(buf, 256,
	         "Audio Driver:  %s\n"
	         "Status:  %s\n"
	         "Sample Rate:  %d\n"
	         "Sample Size:  %u bits\n"
	         "Period Size:  %u samples\n",
	         audio_driver_name,
	         (audio_driver_running()) ? "OK / Running" : "Not Running",
	         sample_rate,
	         (unsigned int)(sizeof(jack_default_audio_sample_t) * 8),
	         sync_info[period].buffer_period_size
	         );
}
