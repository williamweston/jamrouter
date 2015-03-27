/*****************************************************************************
 *
 * midi_event.h
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
#ifndef _MIDI_EVENT_H_
#define _MIDI_EVENT_H_

#include "mididefs.h"


typedef struct keylist {
	unsigned char   midi_key;
	struct keylist  *next;
} KEYLIST;


extern unsigned char           prev_key[MAX_MIDI_QUEUES][16];
extern unsigned char           midi_key[MAX_MIDI_QUEUES][16];
extern unsigned char           last_key[MAX_MIDI_QUEUES][16];

extern KEYLIST                 keylist[MAX_MIDI_QUEUES][16][128];

extern KEYLIST                 *keylist_head[MAX_MIDI_QUEUES][16];

extern volatile MIDI_EVENT     realtime_events[MAX_MIDI_QUEUES];

extern volatile MIDI_EVENT     bulk_event_pool[MAX_MIDI_QUEUES][MIDI_EVENT_POOL_SIZE];

extern volatile EVENT_QUEUE    event_queue[MAX_MIDI_QUEUES][MAX_BUFFER_SIZE];

extern volatile gint           bulk_event_index[MAX_MIDI_QUEUES];

extern unsigned char           keys_in_play[MAX_MIDI_QUEUES];


void init_midi_event_queue(void);
volatile MIDI_EVENT *get_new_midi_event(unsigned char queue_num);
volatile MIDI_EVENT *get_midi_event(unsigned char queue_num,
                                    unsigned short cycle_frame,
                                    unsigned short index);
volatile MIDI_EVENT *dequeue_midi_event(unsigned char queue_num,
                                        unsigned short *last_period,
                                        unsigned short period,
                                        unsigned short cycle_frame);
void queue_midi_event(unsigned short      period,
                      unsigned char       queue_num,
                      volatile MIDI_EVENT *event,
                      unsigned short      cycle_frame,
                      unsigned short      index,
                      unsigned short      copy_event);
void queue_midi_realtime_event(unsigned short period,
                               unsigned char queue_num,
                               unsigned char type,
                               unsigned short cycle_frame,
                               unsigned short index);
void queue_notes_off(unsigned short period,
                     unsigned char queue_num,
                     unsigned char channel,
                     unsigned short cycle_frame,
                     unsigned short index);
void track_note_on(unsigned char queue_num,
                   unsigned char channel,
                   unsigned char midi_note);
void track_note_off(unsigned char queue_num,
                    unsigned char channel,
                    unsigned char midi_note);


#endif /* _MIDI_EVENT_H_ */
