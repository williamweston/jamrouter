/*****************************************************************************
 *
 * midi_event.c
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
#include <asoundlib.h>
#include <glib.h>
#include "jamrouter.h"
#include "timekeeping.h"
#include "mididefs.h"
#include "midi_event.h"
#include "debug.h"
#include "driver.h"


unsigned char           prev_key[MAX_MIDI_QUEUES][16];
unsigned char           midi_key[MAX_MIDI_QUEUES][16];
unsigned char           last_key[MAX_MIDI_QUEUES][16];

KEYLIST                 keylist[MAX_MIDI_QUEUES][16][128];

KEYLIST                 *keylist_head[MAX_MIDI_QUEUES][16];

volatile MIDI_EVENT     realtime_events[MAX_MIDI_QUEUES];

volatile MIDI_EVENT     bulk_event_pool[MAX_MIDI_QUEUES][MIDI_EVENT_POOL_SIZE];

volatile EVENT_QUEUE    event_queue[MAX_MIDI_QUEUES][MAX_BUFFER_SIZE];

volatile gint           bulk_event_index[MAX_MIDI_QUEUES];

unsigned char           keys_in_play[MAX_MIDI_QUEUES];


/*****************************************************************************
 * init_midi_event_queue()
 *****************************************************************************/
void
init_midi_event_queue(void)
{
	volatile MIDI_EVENT *event;
	unsigned short      c;
	unsigned short      e;
	unsigned short      q;

	memset((void *)&(realtime_events[0]), 0,
	       sizeof(MIDI_EVENT)  * MAX_MIDI_QUEUES);
	memset((void *)&(event_queue[0][0]),  0,
	       sizeof(EVENT_QUEUE) * MAX_MIDI_QUEUES * MIDI_EVENT_POOL_SIZE);
	memset((void *)&(bulk_event_pool[0]), 0,
	       sizeof(MIDI_EVENT)  * MAX_MIDI_QUEUES * MIDI_EVENT_POOL_SIZE);

	/* keylist for tracking keys in play */
	for (q = 0; q < MAX_MIDI_QUEUES; q++) {
		keys_in_play[q] = 0;
		for (c = 0; c < 16; c++) {
			prev_key[q][c] = 0xFF;
			midi_key[q][c] = 0xFF;
			last_key[q][c] = 0xFF;
			for (e = 0; e < 128; e++) {
				keylist[q][c][e].midi_key = e & 0x7F;
				keylist[q][c][e].next     = NULL;
			}
		}
	}
	/* realtime event queue for interleaved realtime events */
	for (q = 0; q < MAX_MIDI_QUEUES; q++) {
		event          = &(realtime_events[q]);
		event->type    = MIDI_EVENT_NO_EVENT;
		event->channel = 0x7F;
		event->byte2   = 0;
		event->byte3   = 0;
		event->bytes   = 0;
		event->state   = EVENT_STATE_FREE;
		event->next    = NULL;
	}
	/* main event queue */
	for (q = 0; q < MAX_MIDI_QUEUES; q++) {
		for (e = 0; e < MAX_BUFFER_SIZE; e++) {
			event_queue[q][e].head = NULL;
		}
		for (e = 0; e < MIDI_EVENT_POOL_SIZE; e++) {
			event           = &(bulk_event_pool[q][e]);
			event->type     = MIDI_EVENT_NO_EVENT;
			event->channel  = 0x7F;
			event->byte2    = 0;
			event->byte3    = 0;
			event->bytes    = 0;
			event->state    = EVENT_STATE_FREE;
			event->next     = NULL;
		}
	}
	/* bulk event queue */
	bulk_event_index[A2J_QUEUE] = 0;
	bulk_event_index[J2A_QUEUE] = 0;
}


/*****************************************************************************
 * get_midi_event()
 *****************************************************************************/
volatile MIDI_EVENT *
get_midi_event(unsigned char queue_num,
               unsigned short cycle_frame,
               unsigned short index)
{
	return event_queue[queue_num][index + cycle_frame].head;
}


/*****************************************************************************
 * dequeue_midi_event()
 *****************************************************************************/
volatile MIDI_EVENT *
dequeue_midi_event(unsigned char queue_num,
                   unsigned short *last_period,
                   unsigned short period,
                   unsigned short cycle_frame)
{
	volatile MIDI_EVENT *cur;
	unsigned short      scan_period;
	unsigned short      tx_index;
	unsigned short      j;

	tx_index = (queue_num == J2A_QUEUE) ?
		sync_info[period].tx_index : sync_info[period].output_index;
	cur = event_queue[queue_num][tx_index + cycle_frame].head;

	/* When a period has been missed by the MIDI Tx thread (which has never
	   been observed but still lurks as a potential corner-case if the MIDI Tx
	   thread somehow takes an extra period to return to the dequeuing point),
	   deliver any events still stuck in the queue that should have been
	   delivered last period.  Late dequeuing is a definite synchronization
	   failure.  This check will detect such a failure and keep events from
	   getting stuck in the queue.  While such failure has not been observed
	   during JAMRouter development, this kind of failure is potentially
	   possible with bulk dumps from sequencers that provide no protection
	   against overloading over-the-wire MIDI bandwidth.  Just to be on the
	   safe side, this extra check and scan will prevent any events from being
	   skipped over in the queue. */
	if (period != (((*last_period) + 1) & sync_info[period].period_mask)) {
		for ( scan_period = (unsigned short)
			      (period + sync_info[period].period_mask) &
			      sync_info[period].period_mask;
		      scan_period != period;
		      scan_period = (unsigned short)
			      (scan_period + 1) & sync_info[period].period_mask ) {
			tx_index = (queue_num == J2A_QUEUE) ?
				sync_info[scan_period].tx_index : sync_info[scan_period].output_index;
			for (j = 0; j < sync_info[period].buffer_period_size; j++) {
				if (event_queue[queue_num][tx_index + j].head != NULL ) {
					cur = event_queue[queue_num][tx_index + j].head;
					event_queue[queue_num][tx_index + j].head = NULL;
					JAMROUTER_DEBUG(DEBUG_CLASS_TESTING,
					                DEBUG_COLOR_RED "<"
					                DEBUG_COLOR_YELLOW "LATE"
					                DEBUG_COLOR_RED "> " DEBUG_COLOR_DEFAULT);
					return cur;
				}
			}
		}
	}

	/* Control reaches this point if a) a period was not skipped,
	   and b) there are no events to be dequeued for previous periods.
	   This should be the normal behaviour 100% of the time. */
	*last_period = (unsigned short)(period + sync_info[period].period_mask) &
		sync_info[period].period_mask;

	tx_index = sync_info[period].tx_index;
	event_queue[queue_num][tx_index + cycle_frame].head = NULL;

	return cur;
}


/*****************************************************************************
 * get_new_midi_event()
 *****************************************************************************/
volatile MIDI_EVENT *
get_new_midi_event(unsigned char queue_num)
{
	volatile MIDI_EVENT *new_event;
	guint               new_bulk_index;
	guint               old_bulk_index;

	do {
		old_bulk_index = (guint)(bulk_event_index[queue_num]);
		new_bulk_index = (old_bulk_index + 1) & MIDI_EVENT_POOL_MASK;
	} while (!g_atomic_int_compare_and_exchange(&(bulk_event_index[queue_num]),
	                                            (gint)old_bulk_index,
	                                            (gint)new_bulk_index));
	new_event = &(bulk_event_pool[queue_num][old_bulk_index]);

	new_event->next    = NULL;
	new_event->type    = MIDI_EVENT_NO_EVENT;
	new_event->channel = 0x0;
	new_event->byte2   = 0x0;
	new_event->byte3   = 0x0;
	new_event->bytes   = 0;
	new_event->data[0] = 0xF7;
	new_event->state   = EVENT_STATE_ALLOCATED;

	return new_event;
}


/*****************************************************************************
 * queue_midi_event()
 *****************************************************************************/
void
queue_midi_event(unsigned short         period,
                 unsigned char          queue_num,
                 volatile MIDI_EVENT    *event,
                 unsigned short         cycle_frame,
                 unsigned short         index,
                 unsigned short         copy_event)
{
	volatile MIDI_EVENT      *head        = NULL;
	volatile MIDI_EVENT      *tail        = NULL;
	volatile MIDI_EVENT      *cur         = NULL;
	volatile MIDI_EVENT      *queue_event = event;
	unsigned short           j;

	tail = event_queue[queue_num][index + cycle_frame].head;

	if (cycle_frame > (sync_info[period].buffer_period_size + 1)) {
		JAMROUTER_WARN("%%%%%%  Timing Error:  "
		               "frame=%d > period_size=%d  (adjusting)  %%%%%%\n",
		               cycle_frame, sync_info[period].buffer_period_size);
		cycle_frame = (unsigned short)(sync_info[period].buffer_period_size - 1);
	}
	else if (cycle_frame >= (sync_info[period].buffer_period_size)) {
		cycle_frame = (unsigned short)(sync_info[period].buffer_period_size - 1);
	}

	if (event->type == MIDI_EVENT_ACTIVE_SENSING) {
		switch (active_sensing_mode) {
		case ACTIVE_SENSING_MODE_ON:
			set_active_sensing_timeout(period, queue_num);
			/* intentional fall-through */
		case ACTIVE_SENSING_MODE_DROP:
			event->bytes = 0;
			break;
		case ACTIVE_SENSING_MODE_THRU:
			event->bytes = 1;
			break;
			break;
		}
	}

	/* ignore empty events, or events with no size set */
	if (event->bytes > 0) {
		if (copy_event) {
			queue_event              = get_new_midi_event(queue_num);
			queue_event->type        = event->type;
			queue_event->channel     = event->channel;
			queue_event->byte2       = event->byte2;
			queue_event->byte3       = event->byte3;
			queue_event->bytes       = event->bytes;
			queue_event->float_value = event->float_value;
			if (event->type == MIDI_EVENT_SYSEX) {
				for (j = 0; (j < SYSEX_BUFFER_SIZE) && (j < event->bytes) &&
					     (event->data[j] != sysex_terminator); j++) {
					queue_event->data[j] = event->data[j];
				}
				if (j < SYSEX_BUFFER_SIZE) {
					queue_event->data[j] = sysex_terminator;
				}
				if ( (sysex_extra_terminator != 0xF7) &&
				     (j < (SYSEX_BUFFER_SIZE - 1)) ) {
					queue_event->data[j+1] = sysex_extra_terminator;
				}
			}
		}
		queue_event->next  = NULL;
		queue_event->state = EVENT_STATE_QUEUED;

		/* link to head of list if empty */
		if ((head = event_queue[queue_num][index + cycle_frame].head) == NULL) {
			event_queue[queue_num][index + cycle_frame].head = queue_event;
		}
		else {
			tail = head;
			cur  = tail;
			while (cur != NULL) {
				/* re-use events for same MIDI controller */
				if ( (queue_event->type == MIDI_EVENT_CONTROLLER) &&
				     (cur->type         == MIDI_EVENT_CONTROLLER) &&
				     (cur->channel      == queue_event->channel) &&
				     (cur->controller   == queue_event->controller) ) {
					cur->value         = queue_event->value;
					queue_event->state = EVENT_STATE_ABANDONED;
				}
#ifndef WITHOUT_JUNO
				/* re-use events for same juno sysex controller */
				if ( translate_juno_sysex &&
				     (cur->data[4]      == queue_event->data[4]) &&
				     (queue_event->type == MIDI_EVENT_SYSEX) &&
				     (cur->type         == MIDI_EVENT_SYSEX) &&
				     (cur->data[1]      == 0x41) &&
				     (cur->data[2]      == 0x32) &&
				     (cur->data[6]      == 0xF7) ) {
					cur->data[5]       = queue_event->data[5];
					queue_event->state = EVENT_STATE_ABANDONED;
				}
#endif
				tail = cur;
				cur  = cur->next;
			}

			/* link event to tail, ignoring abandoned events */
			if (queue_event->state != EVENT_STATE_ABANDONED) {
				tail->next = queue_event;
			}
		}
	}

	/* mark events no longer in use as free */
	if (!copy_event && (queue_event->state == EVENT_STATE_ABANDONED)) {
		queue_event->state = EVENT_STATE_FREE;
	}

	/* This memory fence is only an added safety and can be removed when it is
	   determined that the other mfence instructions are in the proper
	   locations for each thread to prevent catastrophic memory reordering. */
	   asm volatile ("mfence; # read/write fence" : : : "memory");

	/* TODO: move most of the work into a real_queue_midi_event() to be used
	   internally by the event queueing system, allowing active sensing checks
	   to be made in a single place (here). */
}


/*****************************************************************************
 * queue_midi_realtime_event()
 *
 * Queues a realtime MIDI event based on event type.  Called when processing
 * MIDI data streams with interleaved MIDI realtime messages.
 *****************************************************************************/
void
queue_midi_realtime_event(unsigned short    period,
                          unsigned char     queue_num,
                          unsigned char     type,
                          unsigned short    cycle_frame,
                          unsigned short    index)
{
	volatile MIDI_EVENT *event = & (realtime_events[queue_num]);

	event->state   = EVENT_STATE_ALLOCATED;
	event->type    = type;
	event->channel = 0xFF;
	event->byte2   = 0;
	event->byte3   = 0;
	event->bytes   = 1;

	queue_midi_event(period, queue_num, event, cycle_frame, index, 1);
}


/*****************************************************************************
 * queue_notes_off()
 *****************************************************************************/
void
queue_notes_off(unsigned short  period,
                unsigned char   queue_num,
                unsigned char   channel,
                unsigned short  cycle_frame,
                unsigned short  index)
{
	KEYLIST             *cur    = keylist_head[queue_num][channel];
	volatile MIDI_EVENT *queue_event;

	/* queue note off event for all notes in play on this queue/channel */
	while (cur != NULL) {
		queue_event           = get_new_midi_event(queue_num);
		if ((queue_num == J2A_QUEUE) && tx_prefer_real_note_off) {
			queue_event->type     = MIDI_EVENT_NOTE_OFF;
			queue_event->velocity = note_off_velocity;
		}
		else {
			queue_event->type     = MIDI_EVENT_NOTE_ON;
			queue_event->velocity = 0;
		}
		queue_event->channel  = channel;
		queue_event->note     = cur->midi_key;
		queue_event->bytes    = 3;
		queue_midi_event(period, queue_num, queue_event, cycle_frame, index, 0);
		track_note_off(queue_num, channel, cur->midi_key);
		cur = g_atomic_pointer_get(&(cur->next));
	}
}


/*****************************************************************************
 * track_note_on()
 *****************************************************************************/
void
track_note_on(unsigned char     queue_num,
              unsigned char     channel,
              unsigned char     midi_note)
{
	KEYLIST     *cur;
	KEYLIST     *prev;
	int         key_in_play = 0;


	/* keep track of previous to newest key pressed! */
	prev_key[queue_num][channel] = midi_key[queue_num][channel];
	midi_key[queue_num][channel] = midi_note & 0x7F;
	last_key[queue_num][channel] = midi_note & 0x7F;

	/* staccato, or no previous notes in play */
	if ( (prev_key[queue_num][channel] == 0xFF) ||
	     (keylist_head[queue_num][channel] == NULL) ) {
		/* put this key at the start of the list */
		keylist[queue_num][channel][midi_note].next = NULL;
		keylist_head[queue_num][channel] =
			&(keylist[queue_num][channel][midi_note]);
	}

	/* legato, or previous notes still in play */
	else {
		/* Link this key to the end of the list,
		   unlinking from the middle if necessary. */
		cur  = keylist_head[queue_num][channel];
		prev = NULL;
		while (cur != NULL) {
			if (cur == &(keylist[queue_num][channel][midi_note])) {
				key_in_play = 1;
				if (prev != NULL) {
					prev->next = cur->next;
				}
			}
			prev = cur;
			cur  = cur->next;
		}
		cur = &(keylist[queue_num][channel][midi_note]);
		/* if there is no end of the list, link it to the head */
		if (prev == NULL) {
			keylist_head[queue_num][channel] = cur;
			JAMROUTER_WARN("*** track_note_on(): [queue %d] found "
			               "previous key in play with no keylist!\n",
			               queue_num);
		}
		else {
			prev->next = cur;
		}
		cur->next = NULL;
	}

	if (!key_in_play) {
		keys_in_play[queue_num]++;
	}

	JAMROUTER_DEBUG(DEBUG_CLASS_MIDI_NOTE,
	                DEBUG_COLOR_LTBLUE "+++%1X:%X:%d+++ " DEBUG_COLOR_DEFAULT,
	                channel, midi_note, keys_in_play[queue_num]);
}


/*****************************************************************************
 * track_note_off()
 *****************************************************************************/
void
track_note_off(unsigned char    queue_num,
               unsigned char    channel,
               unsigned char    midi_note)
{
	KEYLIST     *cur;
	KEYLIST     *prev;
	int         unlink;

	prev_key[queue_num][channel] = midi_key[queue_num][channel];
	midi_key[queue_num][channel] = midi_note & 0x7F;

	/* remove this key from the list and then find the last key */
	prev   = NULL;
	cur    = keylist_head[queue_num][channel];
	unlink = 0;
	while (cur != NULL) {
		/* if note is found, unlink it from the list */
		if (cur->midi_key == midi_note) {
			unlink = 1;
			if (prev != NULL) {
				prev->next = cur->next;
				cur->next  = NULL;
				cur        = prev->next;
			}
			else {
				keylist_head[queue_num][channel] = cur->next;
				cur->next                        = NULL;
				cur = keylist_head[queue_num][channel];
			}
		}
		/* otherwise, on to the next key in the list */
		else {
			prev = cur;
			cur  = cur->next;
		}
	}
	if (unlink) {
		keys_in_play[queue_num]--;
		JAMROUTER_DEBUG(DEBUG_CLASS_MIDI_NOTE,
		                DEBUG_COLOR_LTBLUE "---%1X:%X:%d--- " DEBUG_COLOR_DEFAULT,
		                channel, midi_note, keys_in_play[queue_num]);
	}
	else {
		/* Received note-off w/o corresponding note-on. */
		JAMROUTER_DEBUG(DEBUG_CLASS_MIDI_NOTE,
		                DEBUG_COLOR_RED "---%1X:%X:%d--- " DEBUG_COLOR_DEFAULT,
		                channel, midi_note, keys_in_play[queue_num]);
	}
	/* ignore the note in the note off message if found in list */
	if ((prev != NULL) && (prev->midi_key == midi_note)) {
		cur = keylist_head[queue_num][channel];
		while (cur != NULL) {
			if (cur->midi_key != midi_note) {
				prev = cur;
			}
			cur = cur->next;
		}
	}
	if (prev != NULL) {
		/* set last/current keys in play respective of notes still held */
		last_key[queue_num][channel] = prev->midi_key;
		midi_key[queue_num][channel] = prev->midi_key;
		if (prev->next != NULL) {
			JAMROUTER_WARN("*** track_note_off():  "
			               "Fixed unterminated keytracking list."
			               "  (queue=%d)\n", queue_num);
			prev->next          = NULL;
		}
	}
	/* re-init list if no keys */
	else {
		keylist_head[queue_num][channel] = NULL;
		/* TODO:  Implement hold pedal handling. */
		//if (!hold_pedal[queue_num][channel]) {
		//	midi_key[queue_num][channel] = 0xFF;
		//}
	}
}
