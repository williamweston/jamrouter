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
#ifndef _JAMROUTER_DRIVER_H_
#define _JAMROUTER_DRIVER_H_


#define AUDIO_DRIVER_NONE           0
#define AUDIO_DRIVER_ALSA_PCM       1
#define AUDIO_DRIVER_JACK           2

#define MIDI_DRIVER_NONE            0
#define MIDI_DRIVER_JACK            1
#define MIDI_DRIVER_ALSA_SEQ        2
#define MIDI_DRIVER_RAW_ALSA        3
#define MIDI_DRIVER_RAW_GENERIC     4
#define MIDI_DRIVER_RAW_OSS         5
#define MIDI_DRIVER_RAW_OSS2        6


typedef void *(*THREAD_FUNC)(void *);
typedef int (*DRIVER_FUNC)(void);
typedef int (*DRIVER_INT_FUNC)(int);
typedef void (*DRIVER_VOID_FUNC)(void);
typedef int (*GET_INDEX_FUNC)(int);
typedef void (*CLEANUP_FUNC)(void *);


extern char             audio_driver_status_msg[256];

extern char             *audio_driver_name;
extern char             *midi_driver_name;

extern int              midi_driver;


extern DRIVER_INT_FUNC  audio_init_func;
extern DRIVER_FUNC      audio_start_func;
extern DRIVER_FUNC      audio_stop_func;
extern DRIVER_VOID_FUNC audio_restart_func;
extern DRIVER_VOID_FUNC audio_watchdog_func;
extern THREAD_FUNC      audio_thread_func;

extern DRIVER_FUNC      midi_init_func;
extern DRIVER_FUNC      midi_start_func;
extern DRIVER_FUNC      midi_stop_func;
extern DRIVER_VOID_FUNC midi_restart_func;
extern DRIVER_VOID_FUNC midi_watchdog_func;
extern THREAD_FUNC      midi_rx_thread_func;
extern THREAD_FUNC      midi_tx_thread_func;

extern pthread_mutex_t  jack_audio_ready_mutex;
extern pthread_cond_t   jack_audio_ready_cond;
extern int              jack_audio_ready;
extern int              jack_audio_stopped;

extern pthread_mutex_t  midi_rx_ready_mutex;
extern pthread_cond_t   midi_rx_ready_cond;
extern int              midi_rx_ready;
extern int              midi_rx_stopped;

extern pthread_mutex_t  midi_tx_ready_mutex;
extern pthread_cond_t   midi_tx_ready_cond;
extern int              midi_tx_ready;
extern int              midi_tx_stopped;

extern char             *midi_driver_names[];


void select_midi_driver(char *driver_name, int driver_id);

void init_jack_audio_driver(void);
void init_jack_audio(void);
void start_jack_audio(void);
void wait_jack_audio_start(void);
void wait_jack_audio_stop(void);
void stop_jack_audio(void);
void restart_jack_audio(void);

void init_midi(void);
void start_midi(void);

void start_midi_tx(void);
void start_midi_rx(void);

void stop_midi_rx(void);
void stop_midi_tx(void);

void wait_midi_rx_start(void);
void wait_midi_tx_start(void);

void wait_midi_rx_stop(void);
void wait_midi_tx_stop(void);

void restart_midi(void);

void jamrouter_watchdog(void);
void scan_midi(void);
int  audio_driver_running(void);
void query_audio_driver_status(char *buf);


#endif /* _JAMROUTER_DRIVER_H_ */
