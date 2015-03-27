/******************************************************************************
 *
 * rawmidi.h
 *
 * JAMRouter:  JACK <--> ALSA MIDI Router
 *
 * Copyright (C) 2001-2015 William Weston <william.h.weston@gmail.com>
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
 ******************************************************************************/
#ifndef _JAMROUTER_RAWMIDI_H_
#define _JAMROUTER_RAWMIDI_H_

#include "jamrouter.h"


#if !defined(ENABLE_RAWMIDI_OSS) && !defined(ENABLE_RAWMIDI_ALSA_RAW) && !defined(ENABLE_RAWMIDI_GENERIC)
# error "Please enable at least one Raw MIDI driver to build with Raw MIDI support."
#endif


#ifdef ENABLE_RAWMIDI_OSS
# include <linux/soundcard.h>
#endif

#ifdef ENABLE_RAWMIDI_ALSA_RAW
# include <alsa/asoundlib.h>
#endif


typedef struct rawmidi_info {
	char                *rx_device;
	char                *tx_device;
#if defined(ENABLE_RAWMIDI_OSS) || defined(ENABLE_RAWMIDI_GENERIC)
	int                 rx_fd;
	int                 tx_fd;
#endif
#ifdef ENABLE_RAWMIDI_OSS2
	unsigned char       oss_rx_device;
	unsigned char       oss_tx_device;
#endif
#ifdef ENABLE_RAWMIDI_ALSA_RAW
	snd_rawmidi_t       *rx_handle;
	snd_rawmidi_t       *tx_handle;
#endif
#if defined(RAWMIDI_ALSA_NONBLOCK) || defined(RAWMIDI_GENERIC_NONBLOCK) || defined(RAWMIDI_USE_POLL) || defined(RAWMIDI_OSS_USE_POLL)
	struct pollfd       *pfds;
	int                 npfds;
#endif
	int                 driver;
} RAWMIDI_INFO;


#ifdef ENABLE_RAWMIDI_ALSA_RAW

typedef struct alsa_rawmidi_hw_info {
	int                         card_num;
	int                         device_num;
	int                         subdevice_num;
	int                         connect_request;
	int                         disconnect_request;
	char                        *device_id;
	char                        *device_name;
	char                        *subdevice_name;
	char                        alsa_name[32];
	struct alsa_rawmidi_hw_info *next;
} ALSA_RAWMIDI_HW_INFO;

#endif


extern RAWMIDI_INFO         *rawmidi_info;

extern ALSA_RAWMIDI_HW_INFO *alsa_rawmidi_rx_hw;
extern ALSA_RAWMIDI_HW_INFO *alsa_rawmidi_tx_hw;

extern int                  alsa_rawmidi_hw_changed;


#ifdef ENABLE_RAWMIDI_ALSA_RAW
void alsa_rawmidi_hw_info_free(ALSA_RAWMIDI_HW_INFO *hwinfo);
ALSA_RAWMIDI_HW_INFO *alsa_rawmidi_get_hw_list(snd_rawmidi_stream_t stream_type);
#endif
RAWMIDI_INFO *rawmidi_open(char *rx_device, char *tx_device, int driver);
int rawmidi_close(RAWMIDI_INFO *rawmidi);
int rawmidi_free(RAWMIDI_INFO *rawmidi);
int rawmidi_read(RAWMIDI_INFO *rawmidi, unsigned char *buf, int len);
int rawmidi_write(RAWMIDI_INFO *rawmidi, unsigned char *buf, ssize_t len);
int rawmidi_flush(RAWMIDI_INFO *rawmidi);
void rawmidi_watchdog_cycle(void);
int rawmidi_init(void);
void rawmidi_cleanup(void *arg);
void *raw_midi_rx_thread(void *UNUSED(arg));
void *raw_midi_tx_thread(void *UNUSED(arg));


#endif /* _JAMROUTER_RAWMIDI_H_ */

