/*****************************************************************************
 *
 * jack.h
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
#ifndef _JAMROUTER_JACK_H_
#define _JAMROUTER_JACK_H_

#include <jack/jack.h>
#include "jamrouter.h"


typedef struct jack_port_info {
	jack_port_t             *port;
	char                    *name;
	char                    *type;
	int                     connected;
	short                   connect_request;
	short                   disconnect_request;
	struct jack_port_info   *next;
} JACK_PORT_INFO;


extern jack_client_t        *jack_audio_client;

extern jack_port_t          *midi_input_port;
extern jack_port_t          *midi_output_port;

extern JACK_PORT_INFO       *jack_midi_input_ports;
extern JACK_PORT_INFO       *jack_midi_output_ports;

extern int                  jack_midi_ports_changed;
extern int                  jack_running;

extern char                 *jack_session_uuid;


int jack_process_buffer_no_audio(jack_nframes_t nframes, void *UNUSED(arg));

void jack_port_info_free(JACK_PORT_INFO *portinfo, int follow);
JACK_PORT_INFO *jack_get_midi_port_list(long unsigned int flags);

void jack_shutdown(void *UNUSED(arg));
int  jack_audio_init(int scan);
int  jack_start(void);
int  jack_stop(void);
void jack_restart(void);
void jack_watchdog_cycle(void);


#endif /* _JAMROUTER_JACK_H_ */
