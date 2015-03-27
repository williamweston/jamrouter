/*****************************************************************************
 *
 * juno.h
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
#ifndef _JUNO_H_
#define _JUNO_H_


extern volatile unsigned short  juno_state_bits;
extern volatile unsigned short  juno_state_bits_set;


void translate_from_juno(unsigned short period,
                         unsigned char  queue_num,
                         volatile MIDI_EVENT *event,
                         unsigned short cycle_frame,
                         unsigned short index);
void translate_to_juno(unsigned short period,
                       unsigned char  queue_num,
                       volatile MIDI_EVENT *event,
                       unsigned short cycle_frame,
                       unsigned short index);

#endif /* _JUNO_H_ */
