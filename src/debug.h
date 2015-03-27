/*****************************************************************************
 *
 * debug.h
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
#ifndef _JAMROUTER_DEBUG_H_
#define _JAMROUTER_DEBUG_H_

#include <glib.h>
#include <glib/gatomic.h>
#include "stdio.h"
#include "string.h"
#include "jamrouter.h"


#define DEBUG_MESSAGE_SIZE          256

#define DEBUG_MESSAGE_POOL_SIZE     2048
#define DEBUG_BUFFER_MASK           (DEBUG_MESSAGE_POOL_SIZE - 1)

#define DEBUG_INDEX_TOKEN           2047

#define DEBUG_CLASS_NONE            0
#define DEBUG_CLASS_INIT            (1<<1)
#define DEBUG_CLASS_DRIVER          (1<<2)
#define DEBUG_CLASS_STREAM          (1<<3)
#define DEBUG_CLASS_TIMING          (1<<4)
#define DEBUG_CLASS_TX_TIMING       (1<<5)
#define DEBUG_CLASS_RAW_MIDI        (1<<6)
#define DEBUG_CLASS_MIDI_NOTE       (1<<7)
#define DEBUG_CLASS_MIDI_EVENT      (1<<8)
#define DEBUG_CLASS_SESSION         (1<<9)
#define DEBUG_CLASS_TESTING         (1<<10)
#define DEBUG_CLASS_OSS             (1<<11)
#define DEBUG_CLASS_FULL            (0xFFFF - DEBUG_CLASS_MIDI_EVENT - DEBUG_CLASS_TX_TIMING - DEBUG_CLASS_MIDI_NOTE - DEBUG_CLASS_TESTING - DEBUG_CLASS_OSS)

#define DEBUG_ATTR_RESET            0
#define DEBUG_ATTR_BRIGHT           1
#define DEBUG_ATTR_DIM              2
#define DEBUG_ATTR_UNDERLINE        3
#define DEBUG_ATTR_BLINK            4
#define DEBUG_ATTR_REVERSE          7
#define DEBUG_ATTR_HIDDEN           8

#define DEBUG_COLOR_RED             "\x1B[0;91;49m"
#define DEBUG_COLOR_GREEN           "\x1B[0;32;49m"
#define DEBUG_COLOR_ORANGE          "\x1B[0;33;49m"
#define DEBUG_COLOR_YELLOW          "\x1B[0;93;49m"
#define DEBUG_COLOR_BLUE            "\x1B[0;34;49m"
#define DEBUG_COLOR_LTBLUE          "\x1B[0;94;49m"
#define DEBUG_COLOR_MAGENTA         "\x1B[0;35;49m"
#define DEBUG_COLOR_PINK            "\x1B[0;95;49m"
#define DEBUG_COLOR_CYAN            "\x1B[0;36;49m"
#define DEBUG_COLOR_WHITE           "\x1B[0;37;49m"
#define DEBUG_COLOR_DEFAULT         "\x1B[0;37;49m"


#define DEBUG_STATUS_NONE           0
#define DEBUG_STATUS_QUEUED         1

typedef struct debug_class {
	unsigned long       id;
	char                *name;
} DEBUG_CLASS;

typedef struct debug_msg {
	char                msg[DEBUG_MESSAGE_SIZE];
	volatile gint       status;
} DEBUG_MESSAGE;

typedef struct debug_ringbuffer {
	DEBUG_MESSAGE       msgs[DEBUG_MESSAGE_POOL_SIZE];
	volatile gint       read_index;
	volatile gint       write_index;
	volatile gint       insert_index;
	volatile gint       debug_token;
} DEBUG_RINGBUFFER;


extern DEBUG_RINGBUFFER main_debug_queue;

extern int              debug;
extern int              debug_done;
extern unsigned long    debug_class;

extern DEBUG_CLASS      debug_class_list[16];


#ifdef ENABLE_DEBUG
# define JAMROUTER_ERROR(args...)         jamrouter_warn(args)
# define JAMROUTER_WARN(args...)          jamrouter_warn(args)  
# define JAMROUTER_DEBUG(class, args...)  jamrouter_debug(class, args)
#else /* !ENABLE_DEBUG */
# define JAMROUTER_ERROR(args...)         jamrouter_printf(args)
# define JAMROUTER_WARN(args...)          jamrouter_printf(args)  
# define JAMROUTER_DEBUG(class, args...)  {}
#endif /* !ENABLE_DEBUG */


void jamrouter_printf(const char *format, ...);
void jamrouter_warn(const char *format, ...);
void jamrouter_debug(unsigned int class, const char *format, ...);
void output_pending_debug(void);
void *jamrouter_debug_thread(void *UNUSED(arg));


#endif /* _JAMROUTER_DEBUG_H_ */
