/*****************************************************************************
 *
 * jack_midi.c
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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <jack/jack.h>
#include <jack/midiport.h>
#include "jamrouter.h"
#include "timekeeping.h"
#include "jack.h"
#include "jack_midi.h"
#include "midi_event.h"
#include "debug.h"

#ifndef WITHOUT_JUNO
#include "juno.h"
#endif


/*****************************************************************************
 * jack_process_midi_in()
 *
 * called by jack_process_buffer()
 *****************************************************************************/
void
jack_process_midi_in(unsigned short period, jack_nframes_t nframes)
{
	volatile MIDI_EVENT *out_event;
	void                *port_buf   = jack_port_get_buffer(midi_input_port, nframes);
	jack_midi_event_t   in_event;
	jack_nframes_t      num_events  = jack_midi_get_event_count(port_buf);
	unsigned char       type        = MIDI_EVENT_NO_EVENT;
	unsigned char       channel;
	unsigned short      translated_event;
	unsigned short      e;
	unsigned short      j;
	unsigned short      input_index     = sync_info[period].input_index;
	unsigned short      output_index    = sync_info[period].output_index;
	union {
		short               s;
		unsigned short      u;
	}                   pitchbend;

	pitchbend.u = 0xFFFF;

	/* handle all events for this process cycle */
	for (e = 0; e < num_events; e++) {
		translated_event = 0;
		jack_midi_event_get(&in_event, port_buf, e);
		out_event = get_new_midi_event(J2A_QUEUE);
		/* handle messages with channel number embedded in the first byte */
		if (in_event.buffer[0] < 0xF0) {
			type               = in_event.buffer[0] & 0xF0;
			channel            = in_event.buffer[0] & 0x0F;
			out_event->byte2   = (unsigned char)(in_event.buffer[1]);
			out_event->type    = type;
			out_event->channel = channel;
			/* all channel specific messages except program change and
			   polypressure have 2 bytes following status byte */
			if ( (type == MIDI_EVENT_PROGRAM_CHANGE) ||
			     (type == MIDI_EVENT_POLYPRESSURE)      ) {
				out_event->byte3 = 0x00;
				out_event->bytes = 2;
			}
			else {
				out_event->byte3 = in_event.buffer[2];
				out_event->bytes = 3;
			}
			if ((type == MIDI_EVENT_NOTE_ON) || (type == MIDI_EVENT_NOTE_OFF)) {
				/* translate keys to controller on selected channel */
				if (keymap_tx_channel[channel] != 0xFF) {
					if (out_event->velocity == 0) {
						out_event->type       = MIDI_EVENT_NO_EVENT;
						out_event->channel    = 0xFF;
						out_event->byte2      = 0;
						out_event->byte3      = 0;
						out_event->bytes      = 0;
						translated_event      = 0;
					}
					else {
						out_event->type       = MIDI_EVENT_CONTROLLER;
						out_event->channel    = keymap_tx_channel[channel];
						out_event->controller = keymap_tx_controller[channel];
						out_event->value      = in_event.buffer[1];
						out_event->bytes      = 3;
						translated_event      = 1;
						channel               = keymap_tx_channel[channel];
					}
				}
				/* translate keys to pitchbend on selected channel */
				else if (pitchmap_tx_channel[channel] != 0xFF) {
					if ( (out_event->velocity > 0) &&
					     (out_event->note >= (pitchmap_center_note[channel]
					                          - pitchmap_bend_range[channel])) &&
					     (out_event->note <= (pitchmap_center_note[channel]
					                          + pitchmap_bend_range[channel])) ) {
						pitchbend.s = (short)(((double)8191.0 *
						                       (double)(out_event->note -
						                                pitchmap_center_note[channel]) /
						                       (double)(pitchmap_bend_range[channel])) +
						                      (double)8192.0);
						pitchbend.u &= 0x3FFF;
						out_event->type     = MIDI_EVENT_PITCHBEND;
						out_event->channel  = pitchmap_tx_channel[channel];
 						out_event->lsb      = pitchbend.u & 0x7F;
						out_event->msb      = (pitchbend.u >> 7) & 0x7F;
						out_event->bytes    = 3;
						translated_event    = 1;
						channel             = pitchmap_tx_channel[channel];
					}
					else {
						out_event->type     = MIDI_EVENT_NO_EVENT;
						out_event->channel  = 0xFF;
						out_event->byte2    = 0;
						out_event->byte3    = 0;
						out_event->bytes    = 0;
						translated_event    = 0;
					}
				}
				/* note off tracking / translation */
				else if (out_event->velocity == 0) {
					/* convert note-off to more common velicity=0 note-on messages */
					out_event->type = MIDI_EVENT_NOTE_ON;
					/* translate back to optional alternate note off velocity */
					out_event->velocity = note_off_velocity;
					track_note_off(J2A_QUEUE, channel, out_event->note);
					/* translate last note off into all-notes-off controller */
					/* MIDI Tx thread will ignore other note-off messages queued */
					/* for the same cycle frame to save MIDI bandwidth. */
					if (tx_prefer_all_notes_off && (keys_in_play[J2A_QUEUE] == 0)) {
						out_event->type       = MIDI_EVENT_CONTROLLER;
						out_event->channel    = channel;
						out_event->controller = MIDI_CONTROLLER_ALL_NOTES_OFF;
						out_event->value      = 0x0;
						out_event->bytes      = 3;
					}
					/* tx real note-off instead of note-on-velocity-0 */
					else if (tx_prefer_real_note_off) {
						out_event->type = MIDI_EVENT_NOTE_OFF;
					}
				}
				/* note on tracking / translation */
				else if (out_event->type == MIDI_EVENT_NOTE_ON) {
					if (note_on_velocity != 0x0) {
						out_event->velocity = note_on_velocity;
					}
					track_note_on(J2A_QUEUE, channel, out_event->note);
				}
			}
			/* translate pitchbend to controller on alternate channel */
			else if ( (pitchcontrol_tx_channel[channel] != 0xFF) &&
			          (out_event->type == MIDI_EVENT_PITCHBEND) ) {
				out_event->type       = MIDI_EVENT_CONTROLLER;
				out_event->channel    = pitchcontrol_tx_channel[channel];
				out_event->controller = pitchcontrol_controller[channel];
				/* controller value is already set from pitchbend msb. */
				/* message size is the same and does not need to be reset. */
				pitchbend.u      = out_event->msb;
				translated_event = 1;
				channel          = pitchcontrol_tx_channel[channel];
			}
			/* echo translated events back to jack tx. */
			if (echotrans && translated_event && (out_event->bytes > 0)) {
				queue_midi_event(period, A2J_QUEUE, out_event,
				                 (unsigned short)(in_event.time), output_index, 1);
			}
#ifndef WITHOUT_JUNO
			/* translate controllers to Juno-106 sysex */
			translate_to_juno(period, J2A_QUEUE, out_event,
			                  (unsigned short)(in_event.time), output_index);
#endif
		}
		/* handle other messages (sysex / clock / automation / etc) */
		else {
			type = in_event.buffer[0];
			out_event->type = (unsigned char)(in_event.buffer[0]);
			switch (in_event.buffer[0]) {
			case MIDI_EVENT_SYSEX:          // 0xF0
				out_event->bytes = (unsigned int)(in_event.size);
				if (out_event->bytes > SYSEX_BUFFER_SIZE) {
					out_event->bytes = SYSEX_BUFFER_SIZE;
				}
				memcpy((void *)(out_event->data), in_event.buffer, out_event->bytes);
				/* convert end-sysex byte for obscure hardware. */
				if (out_event->data[in_event.size - 1] != sysex_terminator) {
					if (out_event->data[in_event.size - 1] == 0xF7) {
						out_event->data[in_event.size - 1] = sysex_terminator;
					}
					else {
						out_event->data[in_event.size] = sysex_terminator;
						out_event->bytes++;
						if ( (out_event->bytes < SYSEX_BUFFER_SIZE) &&
						     (sysex_extra_terminator != 0xF7) ) {
							out_event->data[in_event.size + 1] = sysex_extra_terminator;
							out_event->bytes++;
						}
					}
				}
				break;
			/* 3 byte system messages */
			case MIDI_EVENT_SONGPOS:        // 0xF2
				out_event->byte2 = in_event.buffer[1];
				out_event->byte3 = in_event.buffer[2];
				out_event->bytes = 3;
				break;
			/* 2 byte system messages */
			case MIDI_EVENT_MTC_QFRAME:     // 0xF1
			case MIDI_EVENT_SONG_SELECT:    // 0xF3
				out_event->byte2 = in_event.buffer[1];
				out_event->bytes = 2;
				break;
			/* 1 byte realtime messages */
			case MIDI_EVENT_BUS_SELECT:     // 0xF5
			case MIDI_EVENT_TUNE_REQUEST:   // 0xF6
			case MIDI_EVENT_END_SYSEX:      // 0xF7
			case MIDI_EVENT_TICK:           // 0xF8
			case MIDI_EVENT_START:          // 0xFA
			case MIDI_EVENT_CONTINUE:       // 0xFB
			case MIDI_EVENT_STOP:           // 0xFC
			case MIDI_EVENT_EXTENDED_FD:    // 0xFD
			case MIDI_EVENT_ACTIVE_SENSING: // 0xFE
			case MIDI_EVENT_SYSTEM_RESET:   // 0xFF
				out_event->bytes = 1;
				break;
			default:
				break;
			}
		} /* else() */

		/* queue event. */
		queue_midi_event(period, J2A_QUEUE, out_event,
		                 (unsigned short)(in_event.time), input_index, 0);

		if (debug_class & DEBUG_CLASS_STREAM) {
			JAMROUTER_DEBUG(DEBUG_CLASS_TESTING, "\n");
			for (j = 0; j < in_event.size; j++) {
				JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
				                DEBUG_COLOR_ORANGE "%02X " DEBUG_COLOR_DEFAULT,
				                in_event.buffer[j]);
			}
		}
		JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
		                DEBUG_COLOR_ORANGE "[%d] " DEBUG_COLOR_DEFAULT,
		                in_event.time);

	} /* for() */

	//jack_midi_clear_buffer(port_buf);

	/* now that all events for this cycle are handled, check for an active
	   sensing timeout. */
	/* a real timeout has occurred when there are _no_ midi events. */
	if ( (num_events == 0) &&
	     (check_active_sensing_timeout(period, J2A_QUEUE)
	      == ACTIVE_SENSING_STATUS_TIMEOUT) ) {
		for (j = 0; j < 16; j++) {
			queue_notes_off(period, J2A_QUEUE, (unsigned char)(j),
			                0, input_index);
		}
	}
}

/*****************************************************************************
 * jack_process_midi_out()
 *
 * called by jack_process_buffer() or jack_midi_tx_thread()
 *****************************************************************************/
void
jack_process_midi_out(unsigned short period, jack_nframes_t nframes)
{
	volatile MIDI_EVENT     *event;
	volatile MIDI_EVENT     *next;
	void                    *port_buf = jack_port_get_buffer(midi_output_port, nframes);
	volatile unsigned char  *p;
	volatile unsigned char  *q;
	jack_midi_data_t        *buffer;
	unsigned short          cycle_frame;
	unsigned short          j;
	unsigned short          last_period =
		((unsigned short)(period + sync_info[period].period_mask) & sync_info[period].period_mask);

	jack_midi_clear_buffer(port_buf);

	for (cycle_frame = 0; cycle_frame < sync_info[period].buffer_period_size; cycle_frame++) {
		event = dequeue_midi_event(A2J_QUEUE, &last_period, period, cycle_frame);
		
		while ((event != NULL) && (event->state == EVENT_STATE_QUEUED)) {

			if (event->bytes > 0) {
				buffer = jack_midi_event_reserve(port_buf, cycle_frame, event->bytes);

				/* handle messages with channel number embedded in the first byte */
				if (event->type < 0xF0) {
					buffer[0] = (jack_midi_data_t)((event->type & 0xF0) |
					                               (event->channel & 0x0F));
					buffer[1] = (jack_midi_data_t)(event->byte2);
					/* all channel specific messages except program change and
					   polypressure have 2 bytes following status byte */
					if ((event->type != 0xC0) && (event->type != 0xD0)) {
						buffer[2] = (jack_midi_data_t)(event->byte3);
					}
					else {
						buffer[2] = (jack_midi_data_t)0x0;
					}
				}

 				/* handle system (non-channel) messages */
				else {
					buffer[0] = (jack_midi_data_t)(event->type);
					switch (event->type) {
					case MIDI_EVENT_SYSEX:          // 0xF0
						p = (volatile unsigned char *)(event->data);
						q = buffer;
						*q = 0xF0;
						while (*p != 0xF7) {
							*q++ = *p++;
						}
						*q++ = 0xF7;
						break;
						/* 3 byte system messages */
					case MIDI_EVENT_SONGPOS:        // 0xF2
						buffer[1] = (jack_midi_data_t)(event->byte2);
						buffer[2] = (jack_midi_data_t)(event->byte3);
						break;
						/* 2 byte system messages */
					case MIDI_EVENT_MTC_QFRAME:     // 0xF1
					case MIDI_EVENT_SONG_SELECT:    // 0xF3
						buffer[1] = (jack_midi_data_t)(event->byte2);
						break;
						/* 1 byte realtime messages */
					case MIDI_EVENT_BUS_SELECT:     // 0xF5
					case MIDI_EVENT_TUNE_REQUEST:   // 0xF6
					case MIDI_EVENT_END_SYSEX:      // 0xF7
					case MIDI_EVENT_TICK:           // 0xF8
					case MIDI_EVENT_START:          // 0xFA
					case MIDI_EVENT_CONTINUE:       // 0xFB
					case MIDI_EVENT_STOP:           // 0xFC
					case MIDI_EVENT_ACTIVE_SENSING: // 0xFE
					case MIDI_EVENT_SYSTEM_RESET:   // 0xFF
						break;
						/* The following are internal message types */
#ifdef MIDI_CLOCK_SYNC
					case MIDI_EVENT_CLOCK:
#endif /* MIDI_CLOCK_SYNC */
					case MIDI_EVENT_BPM_CHANGE:
					case MIDI_EVENT_PHASE_SYNC:
					case MIDI_EVENT_PARAMETER:
					case MIDI_EVENT_NOTES_OFF:
					default:
						JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
						                DEBUG_COLOR_PINK ">%02X< " DEBUG_COLOR_DEFAULT,
						                event->type);
						break;
					}
				} /* else() */
				if (debug_class & DEBUG_CLASS_STREAM) {
					for (j = 0; j < event->bytes; j++) {
						JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
						                DEBUG_COLOR_PINK "%02X " DEBUG_COLOR_DEFAULT,
						                buffer[j]);
					}
				}
				JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
				                DEBUG_COLOR_PINK "[%d] " DEBUG_COLOR_DEFAULT,
				                cycle_frame);
			} /* if (bytes > 0) */

			/* keep track of next event */
			next = (MIDI_EVENT *)(event->next);

			/* Clear event. */
			event->type    = 0;
			event->channel = 0;
			event->byte2   = 0;
			event->byte3   = 0;
			event->next    = NULL;
			event->state   = EVENT_STATE_FREE;

			/* process next event next while() iteration */
			event = next;

		} /* while */
	}
}
