/*****************************************************************************
 *
 * juno.c
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
#include "mididefs.h"
#include "midi_event.h"
#include "juno.h"


volatile unsigned short  juno_state_bits      = 0x00;
volatile unsigned short  juno_state_bits_set  = 0x00;


/*****************************************************************************
 * translate_from_juno()
 *****************************************************************************/
void
translate_from_juno(unsigned short period,
                    unsigned char  queue_num,
                    volatile MIDI_EVENT *event,
                    unsigned short cycle_frame,
                    unsigned short index)
{
	int     j;

	/* special handling for Juno-106 sysex */
	if ((event->type == MIDI_EVENT_SYSEX) && translate_juno_sysex) {
		/* sysex controller message conversion */
		if ( (event->data[1] == 0x41) &&
		     (event->data[2] == 0x32) && (event->data[6] == 0xF7) ) {
			event->type       = MIDI_EVENT_CONTROLLER;
			event->channel    = event->data[3] & 0x0F;
			event->controller = (event->data[4] + 0x0E) & 0x7F;
			event->value      = event->data[5] & 0x7F;
			event->bytes      = 3;
		}
		/* sysex patch message translation */
		else if ( (event->data[1] == 0x41) &&
		          ((event->data[2] == 0x30) || (event->data[2] == 0x31)) &&
		          (event->data[23] == 0xF7) ) {
			event->type       = MIDI_EVENT_CONTROLLER;
			event->channel    = event->data[3] & 0x0F;
			event->bytes      = 3;
			/* controller for each sysex data byte in patch dump */
			for (j = 5; j < 23; j++) {
				event->controller = (unsigned char)(j + 9);  /* 5 + 9 = 0x0E */
				event->value      = event->data[j] & 0x7F;
				queue_midi_event(period, queue_num, event,
				                 cycle_frame, index, 1);
			}
			/* controller for each button/switch in the bit-packed fields */
			juno_state_bits =
				(unsigned short)((event->data[21] & 0x7F) |
				                 ((event->data[22] & 0x7F) << 7));
			juno_state_bits_set = 0x3FFF;
			for (j = 0; j < 10; j++) {
				event->controller = (unsigned char)(j + 0x66);
				event->value      = (juno_state_bits & (1 << j)) ? 0x7F : 0x0;
				queue_midi_event(period, queue_num, event,
				                 cycle_frame, index, 1);
			}
			/* controller for HPF Freq 4-position (2-bit) slider switch */
			event->controller = 0x74;
			event->value      = (unsigned char)(((~(juno_state_bits)
			                                          & 0x0C00) >> 5) & 0x60);
			queue_midi_event(period, queue_num, event,
			                 cycle_frame, index, 1);
			/* controller for chorus off/I/II (2-bit) button array */
			event->controller = 0x75;
			event->value      = (unsigned char)((juno_state_bits & 0x60) |
			                                    (~(juno_state_bits) & 0x60));
			queue_midi_event(period, queue_num, event,
			                 cycle_frame, index, 1);
			/* done with sysex translation */
			event->bytes = 0;
		}
	}
}


/*****************************************************************************
 * translate_to_juno()
 *****************************************************************************/
void
translate_to_juno(unsigned short period,
                  unsigned char  queue_num,
                  volatile MIDI_EVENT *event,
                  unsigned short cycle_frame,
                  unsigned short index)
{
	int                 translated_event    = 0;
	unsigned char       bit;

	/* translate controllers to Juno-106 sysex */
	if ( translate_juno_sysex &&
	     (event->type == MIDI_EVENT_CONTROLLER) &&
	     (event->controller != MIDI_CONTROLLER_MODULATION) &&
	     (event->controller != MIDI_CONTROLLER_HOLD_PEDAL) &&
	     (event->controller > 0x0D) &&
	     (event->controller < 0x76) ) {
		switch (event->controller) {
			/* set button state bits */
		case 0x1E:
			juno_state_bits =
				(unsigned short)((juno_state_bits & 0x3F80) |
				                 ((unsigned short)(event->value)));
			juno_state_bits_set |= 0x7F;
			break;
			/* set switch state bits */
		case 0x1F:
			juno_state_bits =
				(unsigned short)((juno_state_bits & 0x7F) |
				                 (event->value << 7));
			juno_state_bits_set |= 0x3F80;
			break;
			/* HPF Freq 4-position 2-bit switch */
		case 0x74:
			juno_state_bits =
				(unsigned short)((juno_state_bits & 0xF3FF) |
				                 ((~(event->value) & 0x60) << 10));
			juno_state_bits_set |= 0x0C00;
			event->controller   = 0x1F;
			event->value        = juno_state_bits & 0x7F;
			break;
			/* chorus off/I/II 2-bit toggle */
		case 0x75:
			juno_state_bits =
				(unsigned short)((juno_state_bits & 0x3F9F) |
				                 (((~(event->value) & 0x20) |
				                   (~(event->value) & 0x40)) & 0x60));
			juno_state_bits_set |= 0x60;
			event->controller   = 0x1E;
			event->value        = juno_state_bits & 0x7F;
			break;
		}
		/* 7-bit controllers */
		if (event->controller < 0x1E) {
			event->type    = MIDI_EVENT_SYSEX;
			event->data[0] = 0xF0;
			event->data[1] = 0x41;
			event->data[2] = 0x32;
			event->data[3] = event->channel & 0x0F;
			event->data[4] = (unsigned char)(event->controller - 0x0E);
			event->data[5] = event->value;
			event->data[6] = 0xF7;
			event->bytes   = 7;
			translated_event = 1;
		}
		/* switch state bit controllers */
		else if ((event->controller > 0x6C) || (event->controller == 0x1F)) {
			if (event->controller == 0x1F) {
				bit = 7;
			}
			else {
				bit = (unsigned char)(event->controller - 0x66);
				juno_state_bits =
					(unsigned short)((juno_state_bits & ~(1 << bit)) |
					                 ((event->value >> 6) << bit));
				juno_state_bits_set |= (unsigned short)((1 << bit) | 0x03FF);
			}
			if ((bit <= 7) && ((juno_state_bits_set & 0x3F80) == 0x3F80)) {
				event->type    = MIDI_EVENT_SYSEX;
				event->data[0] = 0xF0;
				event->data[1] = 0x41;
				event->data[2] = 0x32;
				event->data[3] = event->channel & 0x0F;
				event->data[4] = 0x11;
				event->data[5] = (juno_state_bits >> 7) & 0x7F;
				event->data[6] = 0xF7;
				event->bytes   = 7;
				translated_event = 1;
			}
		}
		/* button state bit controllers */
		else if ((event->controller > 0x65) || (event->controller == 0x1E)) {
			if (event->controller == 0x1E) {
				bit = 0;
			}
			else {
				bit = (unsigned char)(event->controller - 0x66);
				juno_state_bits =
					(unsigned short)((juno_state_bits & ~(1 << bit)) |
					                 ((event->value >> 6) << bit));
				juno_state_bits_set |= (unsigned short)((1 << bit) | 0x03FF);
			}
			if ((bit < 7) && ((juno_state_bits_set & 0x7F) == 0x7F)) {
				event->type    = MIDI_EVENT_SYSEX;
				event->data[0] = 0xF0;
				event->data[1] = 0x41;
				event->data[2] = 0x32;
				event->data[3] = event->channel & 0x0F;
				event->data[4] = 0x10;
				event->data[5] = juno_state_bits & 0x7F;
				event->data[6] = 0xF7;
				event->bytes   = 7;
				translated_event = 1;
			}
		}
	}
	/* echo translated sysex events back to jack tx as well. */
	if (echosysex && translated_event && (event->bytes > 0)) {
		queue_midi_event(period,
		                 queue_num == A2J_QUEUE ? J2A_QUEUE : A2J_QUEUE,
		                 event, cycle_frame, index, 1);
	}
}
