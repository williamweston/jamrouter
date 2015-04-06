/*****************************************************************************
 *
 * jack_dll.h
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
#ifndef _JAMROUTER_JACK_DLL_H_
#define _JAMROUTER_JACK_DLL_H_

#include <jack/jack.h>
#include "jamrouter.h"
#include "timeutil.h"


void get_midi_frame_jack_dll(unsigned short *period,
                             unsigned short *frame);

unsigned short get_midi_period_jack_dll(jack_nframes_t current_frame);

TIMESTAMP *get_frame_time_jack_dll(unsigned short period,
                                   unsigned short frame,
                                   TIMESTAMP *frame_time);

unsigned short sleep_until_next_period_jack_dll(unsigned short period,
                                                TIMESTAMP *now);

void sleep_until_frame_jack_dll(unsigned short period,
                                unsigned short frame);


#endif /* _JAMROUTER_JACK_DLL_H_ */
