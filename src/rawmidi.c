/*****************************************************************************
 *
 * rawmidi.c
 *
 * JAMRouter:  JACK <--> ALSA MIDI Router
 *
 * Copyright (C) 2001-2004,2012-2015 William Weston <william.h.weston@gmail.com>
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
#include <asoundlib.h>
#include "jamrouter.h"
#include "timekeeping.h"
#include "rawmidi.h"
#include "mididefs.h"
#include "midi_event.h"
#include "driver.h"
#include "debug.h"

#ifndef WITHOUT_JUNO
#include "juno.h"
#endif


RAWMIDI_INFO            *rawmidi_info;

int                     rawmidi_sleep_time       = 20000;

unsigned char           midi_realtime_type[32];
int                     realtime_event_count     = 0;

#ifdef ENABLE_RAWMIDI_ALSA_RAW
ALSA_RAWMIDI_HW_INFO    *alsa_rawmidi_rx_hw      = NULL;
ALSA_RAWMIDI_HW_INFO    *alsa_rawmidi_tx_hw      = NULL;

int                     alsa_rawmidi_hw_changed  = 0;

unsigned char           tx_buf[SYSEX_BUFFER_SIZE];


/******************************************************************************
 * alsa_rawmidi_hw_info_free()
 ******************************************************************************/
void
alsa_rawmidi_hw_info_free(ALSA_RAWMIDI_HW_INFO *hwinfo)
{
	ALSA_RAWMIDI_HW_INFO    *cur = hwinfo;
	ALSA_RAWMIDI_HW_INFO    *next;

	while (cur != NULL) {
		if (cur != NULL) {
			if (cur->device_id != NULL) {
				free(cur->device_id);
			}
			if (cur->device_name != NULL) {
				free(cur->device_name);
			}
			if (cur->subdevice_name != NULL) {
				free(cur->subdevice_name);
			}
		}
		next = cur->next;
		free(cur);
		cur = next;
	}
}


/******************************************************************************
 * alsa_rawmidi_hw_list_compare()
 ******************************************************************************/
int
alsa_rawmidi_hw_list_compare(ALSA_RAWMIDI_HW_INFO *a, ALSA_RAWMIDI_HW_INFO *b)
{
	ALSA_RAWMIDI_HW_INFO    *cur_a = a;
	ALSA_RAWMIDI_HW_INFO    *cur_b = b;

	while (cur_a != NULL) {
		if ((cur_b == NULL) ||
		    (cur_a->card_num != cur_b->card_num) ||
		    (cur_a->device_num != cur_b->device_num) ||
		    (cur_a->subdevice_num != cur_b->subdevice_num) ||
		    (strcmp(cur_a->device_id, cur_b->device_id) != 0) ||
		    (strcmp(cur_a->device_name, cur_b->device_name) != 0) ||
		    (strcmp(cur_a->subdevice_name, cur_b->subdevice_name) != 0)) {
			return 1;
		}
		cur_a = cur_a->next;
		cur_b = cur_b->next;
	}
	if (cur_b != NULL) {
		return 1;
	}

	return 0;
}


/******************************************************************************
 * alsa_rawmidi_get_hw_list()
 ******************************************************************************/
ALSA_RAWMIDI_HW_INFO *
alsa_rawmidi_get_hw_list(snd_rawmidi_stream_t stream_type)
{
	ALSA_RAWMIDI_HW_INFO    *head = NULL;
	ALSA_RAWMIDI_HW_INFO    *cur  = NULL;
	ALSA_RAWMIDI_HW_INFO    *hwinfo;
	snd_ctl_t               *handle;
	snd_rawmidi_info_t      *alsaraw_info;
	char                    alsa_name[32];
	int                     card_num;
	int                     device_num;
	unsigned int            subdevice_num;
	unsigned int            num_subdevices;
	int                     err;

	card_num = -1;
	while (snd_card_next(&card_num) >= 0) {
		if (card_num < 0) {
			break;
		}
		sprintf(alsa_name, "hw:%d", card_num);
		if ((err = snd_ctl_open(&handle, alsa_name, 0)) < 0) {
			JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER,
			                "Unable to open ALSA control for card %d: %s\n",
			                card_num, snd_strerror(err));
			continue;
		}
		device_num = -1;
		while (snd_ctl_rawmidi_next_device(handle, &device_num) >= 0) {
			if (device_num < 0) {
				break;
			}
			snd_rawmidi_info_alloca(&alsaraw_info);
			memset(alsaraw_info, 0, snd_rawmidi_info_sizeof());

			snd_rawmidi_info_set_device(alsaraw_info, (unsigned int) device_num);
			snd_rawmidi_info_set_subdevice(alsaraw_info, 0);
			snd_rawmidi_info_set_stream(alsaraw_info, stream_type);

#ifdef ALSA_SCAN_SUBDEVICES
			num_subdevices = snd_rawmidi_info_get_subdevices_count(alsaraw_info);
#else
			num_subdevices = 1;
#endif

			for (subdevice_num = 0; subdevice_num < num_subdevices; subdevice_num++) {
				snd_rawmidi_info_set_subdevice(alsaraw_info, subdevice_num);
				if (snd_ctl_rawmidi_info(handle, alsaraw_info) < 0) {
					JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER,
					                "snd_ctl_rawmidi_info (hw:%d,%d,%d) failed:  %s\n",
					                card_num, device_num, subdevice_num, snd_strerror(err));
					continue;
				}
				if (subdevice_num == 0) {
					num_subdevices = snd_rawmidi_info_get_subdevices_count(alsaraw_info);
				}
				if ((hwinfo = malloc(sizeof(ALSA_RAWMIDI_HW_INFO))) == NULL) {
					jamrouter_shutdown("Out of Memory!\n");
				}
				hwinfo->card_num       = card_num;
				hwinfo->device_num     = device_num;
				hwinfo->subdevice_num  = (int) subdevice_num;
				hwinfo->device_id      = strdup(snd_rawmidi_info_get_id(alsaraw_info));
				hwinfo->device_name    = strdup(snd_rawmidi_info_get_name(alsaraw_info));
				hwinfo->subdevice_name = strdup(snd_rawmidi_info_get_subdevice_name(alsaraw_info));
				if (num_subdevices > 1) {
					snprintf(hwinfo->alsa_name,
					         sizeof(hwinfo->alsa_name),
					         "hw:%d,%d,%d",
					         card_num,
					         device_num,
					         subdevice_num);
				}
				else {
					snprintf(hwinfo->alsa_name,
					         sizeof(hwinfo->alsa_name),
					         "hw:%d,%d",
					         card_num,
					         device_num);
				}
				hwinfo->connect_request    = 0;
				hwinfo->disconnect_request = 0;
				hwinfo->next = NULL;
				if (head == NULL) {
					head = cur = hwinfo;
				}
				else {
					cur->next = hwinfo;
					cur = cur->next;
				}
			}
		}
		snd_ctl_close(handle);
	}

	return head;
}
#endif /* ENABLE_RAWMIDI_ALSA_RAW */


/******************************************************************************
 * rawmidi_open()
 *  char        *device
 *
 * Opens raw MIDI device specified by <device> with semantics for OSS, ALSA,
 * and generic raw MIDI according to the currently selected MIDI driver.
 ******************************************************************************/
RAWMIDI_INFO *
rawmidi_open(char *rx_device, char *tx_device, int driver)
{
	RAWMIDI_INFO        *rawmidi;
	int                 flags          = 0x0;
#if defined(ENABLE_RAWMIDI_GENERIC) && defined(RAWMIDI_FLUSH_ON_START)
	int                 flush_input    = 1;
#endif
#ifdef ENABLE_RAWMIDI_ALSA_RAW
	int                 err;
#endif

	/* allocate mem */
	if ((rawmidi = malloc(sizeof(RAWMIDI_INFO))) == NULL) {
		JAMROUTER_ERROR("Unable to malloc() -- %s\n", strerror(errno));
		return NULL;
	}
	/* memset here causes rawmidi thread to hang.... */
	rawmidi->driver    = driver;
#if defined(ENABLE_RAWMIDI_OSS) || defined(ENABLE_RAWMIDI_GENERIC)
	rawmidi->rx_fd     = -1;
	rawmidi->tx_fd     = -1;
#endif
#if defined(ENABLE_RAWMIDI_ALSA_RAW) || defined(ENABLE_RAWMIDI_GENERIC)
	rawmidi->rx_handle = NULL;
	rawmidi->tx_handle = NULL;
	rawmidi->pfds      = NULL;
	rawmidi->npfds     = 0;
#endif
	rawmidi->rx_device = NULL;
	rawmidi->tx_device = NULL;

	/* use default device appropriate for driver type if none given */
	if ( (rx_device == NULL) || (rx_device[0] == '\0') ||
	     (strcmp(rx_device, "auto") == 0) ) {
		switch (driver) {
#ifdef ENABLE_RAWMIDI_OSS
		case MIDI_DRIVER_RAW_OSS:
			rawmidi->rx_device = strdup(RAWMIDI_OSS_DEVICE);
			break;
#endif
#ifdef ENABLE_RAWMIDI_OSS2
		case MIDI_DRIVER_RAW_OSS2:
			rawmidi->rx_device     = strdup(RAWMIDI_OSS2_DEVICE);
			rawmidi->oss_rx_device = 0x1;
			break;
#endif
#ifdef ENABLE_RAWMIDI_ALSA_RAW
		case MIDI_DRIVER_RAW_ALSA:
			if (alsa_rawmidi_rx_hw != NULL) {
				rawmidi->rx_device = strdup(alsa_rawmidi_rx_hw->alsa_name);
			}
			else {
				rawmidi->rx_device = strdup(RAWMIDI_ALSA_DEVICE);
			}
			break;
#endif
#ifdef ENABLE_RAWMIDI_GENERIC
		case MIDI_DRIVER_RAW_GENERIC:
			rawmidi->rx_device = strdup(RAWMIDI_RAW_DEVICE);
			break;
#endif
		}
	}
#ifdef ENABLE_RAWMIDI_OSS
	else if (driver == MIDI_DRIVER_RAW_OSS) {
		rawmidi->rx_device = strdup(RAWMIDI_OSS_DEVICE);
		rawmidi->oss_rx_device = (unsigned char)(atoi(rx_device) & 0xFF);
	}
#endif
#ifdef ENABLE_RAWMIDI_OSS2
	else if (driver == MIDI_DRIVER_RAW_OSS2) {
		rawmidi->rx_device = strdup(RAWMIDI_OSS2_DEVICE);
		rawmidi->oss_rx_device = (unsigned char)(atoi(rx_device) & 0xFF);
	}
#endif
	else {
		rawmidi->rx_device = strdup(rx_device);
	}

	/* use default device appropriate for driver type if none given */
	if ( (tx_device == NULL) || (tx_device[0] == '\0') ||
	     (strcmp(tx_device, "auto") == 0) ) {
		switch (driver) {
#ifdef ENABLE_RAWMIDI_OSS
		case MIDI_DRIVER_RAW_OSS:
			rawmidi->tx_device = strdup(RAWMIDI_OSS_DEVICE);
			break;
#endif
#ifdef ENABLE_RAWMIDI_OSS2
		case MIDI_DRIVER_RAW_OSS2:
			rawmidi->rx_device     = strdup(RAWMIDI_OSS2_DEVICE);
			rawmidi->oss_rx_device = 0x1;
			break;
#endif
#ifdef ENABLE_RAWMIDI_ALSA_RAW
		case MIDI_DRIVER_RAW_ALSA:
			if (alsa_rawmidi_tx_hw != NULL) {
				rawmidi->tx_device = strdup(alsa_rawmidi_tx_hw->alsa_name);
			}
			else {
				rawmidi->tx_device = strdup(RAWMIDI_ALSA_DEVICE);
			}
			break;
#endif
#ifdef ENABLE_RAWMIDI_GENERIC
		case MIDI_DRIVER_RAW_GENERIC:
			rawmidi->tx_device = strdup(RAWMIDI_RAW_DEVICE);
			break;
#endif
		}
	}
#ifdef ENABLE_RAWMIDI_OSS
	else if (driver == MIDI_DRIVER_RAW_OSS) {
		rawmidi->tx_device = strdup(RAWMIDI_OSS_DEVICE);
		rawmidi->oss_tx_device = (unsigned char)(atoi(tx_device) & 0xFF);
	}
#endif
#ifdef ENABLE_RAWMIDI_OSS2
	else if (driver == MIDI_DRIVER_RAW_OSS2) {
		rawmidi->tx_device = strdup(RAWMIDI_OSS2_DEVICE);
		rawmidi->oss_tx_device = (unsigned char)(atoi(tx_device) & 0xFF);
	}
#endif
	else {
		rawmidi->tx_device = strdup(tx_device);
	}

	/* separate device open methods for OSS/RAW and ALSA insterfaces */
	switch (midi_driver) {
#if defined(ENABLE_RAWMIDI_OSS) || defined(ENABLE_RAWMIDI_OSS2)
	case MIDI_DRIVER_RAW_OSS:
	case MIDI_DRIVER_RAW_OSS2:
# ifdef RAWMIDI_OSS_USE_POLL
		rawmidi_sleep_time = 0;
		flags |= O_NONBLOCK;
# endif
#endif
#ifdef ENABLE_RAWMIDI_GENERIC
	case MIDI_DRIVER_RAW_GENERIC:
# if defined(RAWMIDI_GENERIC_NONBLOCK) || defined(RAWMIDI_USE_POLL)
		rawmidi_sleep_time = 0;
		flags |= O_NONBLOCK;
# endif
#endif
#if defined(ENABLE_RAWMIDI_OSS) || defined(ENABLE_RAWMIDI_GENERIC)
# ifdef RAWMIDI_DUPLEX
		/* if in and out devices are the same,
		   use one read/write descriptor */
		if ((strcmp(rawmidi->rx_device, rawmidi->tx_device) == 0)) {
#  ifdef RAWMIDI_FLUSH_ON_START
			/* open and close nonblocking first to flush */
			JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
			                "Opening Raw MIDI Rx/Tx device '%s' for flush...\n",
			                rawmidi->rx_device);
			if ((rawmidi->rx_fd = open(rawmidi->rx_device,
			                           O_RDWR | O_NONBLOCK)) < 0) {
				JAMROUTER_WARN("Unable to open Raw MIDI device '%s' "
				               "in nonblocking mode -- %s\n",
				               rawmidi->rx_device, strerror(errno));
				flush_input = 0;
			}
			/* flush anything currently in the buffer */
			JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
			                "Flushing Raw MIDI Rx/Tx device '%s'...\n",
			                rawmidi->rx_device);
			if (flush_input) {
				rawmidi_flush(rawmidi);
				close(rawmidi->rx_fd);
			}
#  endif /* RAWMIDI_FLUSH_ON_START */
			/* open device readonly for blocking io */
			JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
			                "Opening Raw MIDI Rx/Tx device '%s' O_RDWR...\n",
			                rawmidi->rx_device);
			if ((rawmidi->rx_fd = open(rawmidi->rx_device,
			                           O_RDWR | flags)) < 0) {
				JAMROUTER_ERROR("Unable to open Raw MIDI Rx/Tx device "
				                "'%s' O_RDWR -- %s\n",
				                rawmidi->rx_device, strerror(errno));
				rawmidi_free(rawmidi);
				return NULL;
			}
			rawmidi->tx_fd = rawmidi->rx_fd;
		}

		/* separate read/write descriptors */
		else
# endif /* RAWMIDI_DUPLEX */
		{
# ifdef RAWMIDI_FLUSH_ON_START
			/* open and close nonblocking first to flush */
			JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
			                "Opening Raw MIDI Rx device '%s' to flush...\n",
			                rawmidi->rx_device);
			if ((rawmidi->rx_fd = open(rawmidi->rx_device,
			                           O_NONBLOCK | O_RDONLY)) < 0) {
				JAMROUTER_WARN("Unable to open Raw MIDI Rx device "
				               "'%s' O_NONBLOCK -- %s\n",
				               rawmidi->rx_device, strerror(errno));
				flush_input = 0;
			}
			JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
			                "Opening Raw MIDI Tx device '%s' to drain...\n",
			                rawmidi->tx_device);
			if ((rawmidi->tx_fd = open(rawmidi->tx_device,
			                           O_NONBLOCK | O_WRONLY)) < 0) {
				JAMROUTER_WARN("Unable to open Raw MIDI Tx device "
				               "'%s' O_NONBLOCK -- %s\n",
				            rawmidi->tx_device, strerror(errno));
				flush_input = 0;
			}
			/* flush anything currently in the buffer */
			if (flush_input) {
				rawmidi_flush(rawmidi);
				close(rawmidi->tx_fd);
				close(rawmidi->rx_fd);
			}
# endif /* RAWMIDI_FLUSH_ON_START */
# if defined(RAWMIDI_GENERIC_NONBLOCK) || defined(RAWMIDI_USE_POLL)
			flags = O_NONBLOCK;
# else
			flags = 0x0;
# endif
			/* open device readonly for blocking io */
			JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
			                "Opening Raw MIDI Rx device '%s' O_RDONLY...\n",
			                rawmidi->rx_device);
			if ((rawmidi->rx_fd = open(rawmidi->rx_device,
			                           flags | O_RDONLY)) < 0) {
				JAMROUTER_ERROR("Unable to open Raw MIDI Rx device "
				                "'%s' for read -- %s\n",
				                rawmidi->rx_device, strerror(errno));
				rawmidi_free(rawmidi);
				return NULL;
			}
			/* open device write only for blocking io */
			JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
			                "Opening Raw MIDI Tx device '%s' O_WRONLY...\n",
			                rawmidi->tx_device);
			if ((rawmidi->tx_fd = open(rawmidi->tx_device,
			                           flags | O_WRONLY)) < 0) {
				JAMROUTER_ERROR("Unable to open Raw MIDI Tx device "
				                "'%s' for read -- %s\n",
				                rawmidi->tx_device, strerror(errno));
				rawmidi_free(rawmidi);
				return NULL;
			}
		}
#ifdef RAWMIDI_USE_POLL
		/* poll descriptor for generic/oss raw rx */
		if ((rawmidi->pfds = malloc(sizeof(struct pollfd))) == NULL) {
			jamrouter_shutdown("Out of Memory!");
		}
		rawmidi->pfds->fd     = rawmidi->rx_fd;
		rawmidi->pfds->events = POLLIN;
		rawmidi->npfds        = 1;
		JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
		                "Opened Generic Raw MIDI device Rx: "
		                "'%s'  Tx: '%s'.\n",
		                rawmidi->rx_device, rawmidi->tx_device);
#endif /* RAWMIDI_USE_POLL */
		break;
#endif /* ENABLE_RAWMIDI_OSS || ENABLE_RAWMIDI_GENERIC */

#ifdef ENABLE_RAWMIDI_ALSA_RAW
	case MIDI_DRIVER_RAW_ALSA:
# ifdef RAWMIDI_ALSA_DUPLEX
		/* if in and out devices are the same,
		   open rx and tx handles at the same time */
		if (strcmp(rawmidi->rx_device, rawmidi->tx_device) == 0) {
			if (rawmidi->rx_device != NULL) {
				JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
				                "Opening ALSA Raw MIDI Rx/Tx device "
				                "'%s' O_RDWR...\n",
				                rawmidi->tx_device);
				if ((err = snd_rawmidi_open(& (rawmidi->rx_handle),
				                            & (rawmidi->tx_handle),
				                            rawmidi->rx_device,
				                            O_RDWR
#  if defined(RAWMIDI_ALSA_NONBLOCK)
				                            | SND_RAWMIDI_NONBLOCK
#  endif
				                            ))) {
					JAMROUTER_ERROR("snd_rawmidi_open %s (rx) / %s (tx) "
					                "failed: %d\n",
					                rawmidi->rx_device,
					                rawmidi->tx_device, err);
					rawmidi->rx_handle = NULL;
					rawmidi_free(rawmidi);
					return NULL;
				}
#  if defined(RAWMIDI_ALSA_NONBLOCK)
				/* set nonblock for rx/tx handle? */
				if (snd_rawmidi_nonblock(rawmidi->rx_handle, 1) != 0) {
					JAMROUTER_ERROR("Unable to set nonblock mode "
					                "for ALSA Raw MIDI.\n");
					rawmidi_free(rawmidi);
					return NULL;
				}
#  endif
			}
		}

		/* open rx and tx separately */
		else
# endif /* RAWMIDI_ALSA_DUPLEX */
		{

			/* open rx */
			if (rawmidi->rx_device != NULL) {
				JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
				                "Opening ALSA Raw MIDI Rx device "
				                "'%s' O_RDONLY...\n",
				                rawmidi->rx_device);
				if ((err = snd_rawmidi_open(& (rawmidi->rx_handle),
				                            NULL,
				                            rawmidi->rx_device,
				                            O_RDONLY
# if defined(RAWMIDI_ALSA_NONBLOCK)
				                            | SND_RAWMIDI_NONBLOCK
# endif
				                            ))) {
					JAMROUTER_ERROR("snd_rawmidi_open %s failed: %d\n",
					             rawmidi->rx_device, err);
					rawmidi->rx_handle = NULL;
					rawmidi_free(rawmidi);
					return NULL;
				}
# if defined(RAWMIDI_ALSA_NONBLOCK)
				JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
				                "Setting ALSA Raw MIDI Rx device "
				                "'%s' to nonblock mode...\n",
				                rawmidi->rx_device);
				if (snd_rawmidi_nonblock(rawmidi->rx_handle, 1) != 0) {
					JAMROUTER_ERROR("Unable to set nonblock mode "
					                "for ALSA Raw MIDI.\n");
					rawmidi_free(rawmidi);
					return NULL;
				}
# endif
			}

			/* open tx */
			if (rawmidi->tx_device != NULL) {
				JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
				                "Opening ALSA Raw MIDI Tx device "
				                "'%s' O_WRONLY...\n",
				                rawmidi->tx_device);
				if ((err = snd_rawmidi_open(NULL,
				                            & (rawmidi->tx_handle),
				                            rawmidi->tx_device,
				                            O_WRONLY
# if defined(RAWMIDI_ALSA_NONBLOCK_TX)
				                            | SND_RAWMIDI_NONBLOCK
# endif
				                            ))) {
					JAMROUTER_ERROR("snd_rawmidi_open %s failed: %d\n",
					             rawmidi->tx_device, err);
					rawmidi->tx_handle = NULL;
					rawmidi_free(rawmidi);
					return NULL;
				}
# if defined(RAWMIDI_ALSA_NONBLOCK_TX)
				if (snd_rawmidi_nonblock(rawmidi->tx_handle, 1) != 0) {
					JAMROUTER_ERROR("Unable to set nonblock mode "
					                "for ALSA Raw MIDI.\n");
					rawmidi_free(rawmidi);
					return NULL;
				}
# endif
			}
			
		}
# if defined(RAWMIDI_ALSA_NONBLOCK) || defined(RAWMIDI_USE_POLL)
		/* in case we opened nonblocking, we need our poll descriptors */
		if ( (rawmidi->npfds =
		      snd_rawmidi_poll_descriptors_count(rawmidi->rx_handle)) > 0 ) {
			if ((rawmidi->pfds =
			     malloc((size_t)(rawmidi->npfds *
			                     (int) sizeof(struct pollfd)))) == NULL) {
				jamrouter_shutdown("Out of memory!\n");
			}
			if (snd_rawmidi_poll_descriptors(rawmidi->rx_handle, rawmidi->pfds,
			                                 (unsigned int)rawmidi->npfds)
			    <= 0) {
				JAMROUTER_ERROR("No ALSA rawmidi descriptors to poll.\n");
				rawmidi_free(rawmidi);
				return NULL;
			}
		}
		rawmidi_sleep_time = 0;
# endif /* RAWMIDI_ALSA_NONBLOCK || RAWMIDI_USE_POLL */
		JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
		                "Opened ALSA Raw MIDI device Rx: '%s'  Tx: '%s'.\n",
		                rawmidi->rx_device, rawmidi->tx_device);
		break;
#endif /* ENABLE_RAWMIDI_ALSA_RAW */
	default:
		break;
	}

	/* Return the abstracted rawmidi device */
	return rawmidi;
}


/******************************************************************************
 * rawmidi_close()
 *  RAWMIDI_INFO *rawmidi
 *
 * Closes the rawmidi interfaces and upon success clears the file
 * descriptors held in the internal structure.  Returns zero on
 * success, or -1 on error.
 ******************************************************************************/
int
rawmidi_close(RAWMIDI_INFO *rawmidi)
{
	int     retval2 = -1;
	int     retval1 = -1;

	/* separate device open methods for OSS and ALSA insterfaces */
	switch (midi_driver) {
#ifdef ENABLE_RAWMIDI_OSS
	case MIDI_DRIVER_RAW_OSS:
#endif
#ifdef ENABLE_RAWMIDI_GENERIC
	case MIDI_DRIVER_RAW_GENERIC:
#endif
#if defined(ENABLE_RAWMIDI_OSS) || defined(ENABLE_RAWMIDI_GENERIC)
		/* sanity checks */
		if (rawmidi == NULL) {
			return -1;
		}
		if ((rawmidi->rx_fd < 0) && (rawmidi->tx_fd < 0)) {
			return -1;
		}

		/* close device */
		if ( (rawmidi->rx_fd >= 0) &&
		     ((retval1 = close(rawmidi->rx_fd)) == 0) ) {
			rawmidi->rx_fd = -1;
		}
		if ( (rawmidi->tx_fd >= 0) &&
		     ((retval2 = close(rawmidi->tx_fd)) == 0) ) {
			rawmidi->tx_fd = -1;
		}
		if ((retval1 & retval2) == 0) {
			return 0;
		}

		return -1;
#endif /* ENABLE_RAWMIDI_OSS || ENABLE_RAWMIDI_GENERIC */
#ifdef ENABLE_RAWMIDI_ALSA_RAW
	case MIDI_DRIVER_RAW_ALSA:
		/* sanity checks */
		if (rawmidi == NULL) {
			return -1;
		}
		if ((rawmidi->rx_handle == NULL) && (rawmidi->tx_handle == NULL)) {
			return -1;
		}

		/* close device handle(s) */
		if ( (rawmidi->rx_handle != NULL) &&
		     ((retval1 = snd_rawmidi_close(rawmidi->rx_handle)) == 0) ) {
			rawmidi->rx_handle = NULL;
		}
		if ( (rawmidi->tx_handle != NULL) &&
		     ((retval2 = snd_rawmidi_close(rawmidi->tx_handle)) == 0) ) {
			rawmidi->tx_handle = NULL;
		}

		if ((retval1 & retval2) == 0) {
			return 0;
		}

		return -1;
#endif /* ENABLE_RAWMIDI_ALSA_RAW */
	default:
		break;
	}

	return retval1 & retval2;
}


/******************************************************************************
 * rawmidi_free()
 *  RAWMIDI_INFO *rawmidi
 *
 * Frees all memory and descriptors associated with a rawmidi
 * structure.  Returns zero on success, or -1 on error.
 ******************************************************************************/
int
rawmidi_free(RAWMIDI_INFO *rawmidi)
{
	/* close descriptors */
	rawmidi_close(rawmidi);

	/* free allocated strings */
	if (rawmidi->rx_device != NULL) {
		free(rawmidi->rx_device);
	}
	if (rawmidi->tx_device != NULL) {
		free(rawmidi->tx_device);
	}

	if (rawmidi->pfds != NULL) {
		free(rawmidi->pfds);
	}

	/* free allocated structure */
	free(rawmidi);

	return 0;
}


/******************************************************************************
 * rawmidi_read()
 *  RAWMIDI_INFO    *rawmidi
 *  char            *buf
 *  int             len
 *
 * Reads <len> bytes from the rawmidi input device and places the
 * contents in <buf>.  Much like the read() (2) system call, <buf>
 * must be of sufficient size for the requested length.  Return value
 * is the number of raw midi bytes read, or -1 on error.
 ******************************************************************************/
int
rawmidi_read(RAWMIDI_INFO *rawmidi, unsigned char *buf, int len)
{
#if defined(ENABLE_RAWMIDI_OSS) || defined(ENABLE_RAWMIDI_OSS2)
	unsigned char           ibuf[8];
	ssize_t                 bytes;
#endif /* ENABLE_RAWMIDI_OSS || ENABLE_RAWMIDI_OSS2 */
	int                     bytes_read      = 0;
#if defined(ENABLE_RAWMIDI_OSS) || (defined(ENABLE_RAWMIDI_ALSA_RAW) && defined(RAWMIDI_ALSA_MULTI_BYTE_IO))
	static unsigned char    read_buf[256];
	static int              buf_index       = 0;
	static ssize_t          buf_available   = 0;
	int                     output_index    = 0;
#endif /* ENABLE_RAWMIDI_ALSA_RAW && RAWMIDI_ALSA_MULTI_BYTE_IO */

	switch (midi_driver) {

		/* for OSS, filter event type and extract raw data from event quads */
#if defined(ENABLE_RAWMIDI_OSS) || defined(ENABLE_RAWMIDI_OSS2)
	case MIDI_DRIVER_RAW_OSS:
	case MIDI_DRIVER_RAW_OSS2:
		if (buf_available > 0) {
			while ((buf_index < buf_available) && (output_index < len)) {
				JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
				                DEBUG_COLOR_CYAN "%02X "
				                DEBUG_COLOR_DEFAULT,
				                read_buf[buf_index]);
				buf[output_index++] = read_buf[buf_index++];
				if (output_index == len) {
					return len;
				}
			}
		}
		buf_available = 0;
		buf_index = 0;
		/* strip raw midi message out of larger OSS message */
		while (!midi_rx_stopped && !pending_shutdown && (buf_available < len)) {
# ifdef RAWMIDI_OSS_USE_POLL
			if (poll(rawmidi_info->pfds, (nfds_t) rawmidi_info->npfds, 1) > 0)
# endif /* RAWMIDI_OSS_USE_POLL */
			{
				/* read one event quad at a time */
				if ((bytes = read(rawmidi->rx_fd, ibuf, 4)) > 0) {
					JAMROUTER_DEBUG(DEBUG_CLASS_OSS,
					                DEBUG_COLOR_RED "%d "
					                DEBUG_COLOR_CYAN "%02X %02X %02X %02X %02X %02X %02X %02X "
					                DEBUG_COLOR_DEFAULT,
					                (int)bytes,
					                ibuf[0], ibuf[1], ibuf[2], ibuf[3],
					               ibuf[4], ibuf[5], ibuf[6], ibuf[7]);
					/* new 8-byte /dev/sequencer2 interface */
					if ( (bytes == 8) && (ibuf[0] > 0x80)
					     && (ibuf[1] == rawmidi->oss_rx_device)
					     ) {
						switch (ibuf[0]) {
						case 0x92:
							switch (ibuf[2]) {
							case 0xC0:
							case 0xD0:
								read_buf[bytes_read++] = ibuf[2];
								read_buf[bytes_read++] = ibuf[6];
								break;
							default:
								read_buf[bytes_read++] = ibuf[2];
								read_buf[bytes_read++] = ibuf[4];
								read_buf[bytes_read++] = ibuf[6];
								break;
							}
							break;
						case 0x93:
							read_buf[bytes_read++] = ibuf[2];
							read_buf[bytes_read++] = ibuf[4];
							read_buf[bytes_read++] = ibuf[5];
							break;
						case 0x94:
							for (bytes = 2; bytes < 8; bytes++) {
								read_buf[bytes_read++] = ibuf[bytes];
								if (ibuf[bytes] == 0xF7) {
									break;
								}
							}
							break;
						}
					}
					/* old 4-byte /dev/sequencer interface */
					else if ( (bytes == 4) && (ibuf[0] == SEQ_MIDIPUTC)
					          //&& (ibuf[1] == rawmidi->oss_rx_device)
					          ) {
						/* only take the second of every four bytes */
						read_buf[bytes_read++] = ibuf[1];
					}
					else {
						continue;
					}

					buf_index = 0;
					buf_available = bytes_read;
					while ((buf_index < bytes_read) && (output_index < len)) {
						JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
						                DEBUG_COLOR_CYAN "%02X "
						                DEBUG_COLOR_DEFAULT,
						                read_buf[buf_index]);
						buf[output_index++] = read_buf[buf_index++];
					}
					bytes_read = output_index;
				}
				else {
# ifdef RAWMIDI_OSS_USE_POLL
					JAMROUTER_ERROR("Unable to read from OSS MIDI device "
					                "'%s' -- %s!\n",
					                rawmidi->rx_device, strerror(errno));
# else /* !RAWMIDI_OSS_USE_POLL */
					jamrouter_nanosleep(rawmidi_sleep_time);
# endif /* !RAWMIDI_OSS_USE_POLL */
				}
			}
		}
		break;
#endif /* ENABLE_RAWMIDI_OSS || ENABLE_RAWMIDI_OSS2 */

		/* for ALSA, single byte and multi-byte seem dependable */
#ifdef ENABLE_RAWMIDI_ALSA_RAW
	case MIDI_DRIVER_RAW_ALSA:
# ifdef RAWMIDI_ALSA_MULTI_BYTE_IO
		if (buf_available > 0) {
			while ((buf_index < buf_available) && (output_index < len)) {
				JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
				                DEBUG_COLOR_CYAN "%02X "
				                DEBUG_COLOR_DEFAULT,
				                read_buf[buf_index]);
				buf[output_index++] = read_buf[buf_index++];
			}
		}
		if (!midi_rx_stopped && !pending_shutdown && (output_index < len)) {
#  if defined(RAWMIDI_ALSA_NONBLOCK) || defined(RAWMIDI_USE_POLL)
			if (poll(rawmidi_info->pfds, (nfds_t) rawmidi_info->npfds, 1) > 0)
#  endif
			{
				if ((buf_available = snd_rawmidi_read(rawmidi->rx_handle,
				                                      read_buf, 256)) < 1) {
					JAMROUTER_ERROR("Unable to read from ALSA Raw MIDI "
					                "device '%s'!\n",
					                rawmidi->rx_device);
				}
				buf_index = 0;
				while ((buf_index < buf_available) && (output_index < len)) {
					JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
					                DEBUG_COLOR_CYAN "%02X "
					                DEBUG_COLOR_DEFAULT,
					                read_buf[buf_index]);
					buf[output_index++] = read_buf[buf_index++];
				}
			}
		}
		bytes_read = output_index;

# else /* !RAWMIDI_ALSA_MULTI_BYTE_IO */
		bytes_read = 0;
		while (!midi_rx_stopped && !pending_shutdown &&
		       (bytes_read < len)) {
#  if defined(RAWMIDI_ALSA_NONBLOCK) || defined(RAWMIDI_USE_POLL)
			if (poll(rawmidi_info->pfds,
			         (nfds_t) rawmidi_info->npfds, 1) > 0)
#  endif
			{
				if (snd_rawmidi_read(rawmidi->rx_handle,
				                     &(buf[bytes_read]), 1) != 1) {
					JAMROUTER_ERROR("Unable to read from ALSA Raw MIDI "
					                "device '%s'!\n",
					                rawmidi->rx_device);
					break;
				}
				else {
					JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
					                DEBUG_COLOR_CYAN "%02X "
					                DEBUG_COLOR_DEFAULT,
					                buf[bytes_read]);
					bytes_read++;
				}
			}
		}
# endif /* RAWMIDI_ALSA_MULTI_BYTE_IO */
		break;
#endif /* ENABLE_RAWMIDI_ALSA_RAW */

		/* for raw interfaces, read one byte at a time */
#ifdef ENABLE_RAWMIDI_GENERIC
	case MIDI_DRIVER_RAW_GENERIC:
		/* read one byte at a time to make some interfaces happy */
		bytes_read = 0;
		while (!midi_rx_stopped && !pending_shutdown &&
		       (bytes_read < len)) {
# ifdef RAWMIDI_GENERIC_NONBLOCK
#  ifdef RAWMIDI_USE_POLL
			if (poll(rawmidi_info->pfds, (nfds_t)
			         rawmidi_info->npfds, 1) > 0)
#  endif /* RAWMIDI_USE_POLL */
			{
				if (read(rawmidi->rx_fd, &buf[bytes_read], 1) == 1) {
					JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
					                DEBUG_COLOR_CYAN "%02X "
					                DEBUG_COLOR_DEFAULT,
					                buf[bytes_read]);
					bytes_read++;
				}
				else {
					jamrouter_nanosleep(rawmidi_sleep_time);
				}
			}
# else /* !RAWMIDI_GENERIC_NONBLOCK */
			if (!midi_rx_stopped && !pending_shutdown
#  ifdef RAWMIDI_USE_POLL
			    && (poll(rawmidi_info->pfds,
			             (nfds_t)rawmidi_info->npfds, 0) > 0)
#  endif /* RAWMIDI_USE_POLL */
			    ) {
				if (read(rawmidi->rx_fd, &buf[bytes_read], 1) == 1) {
					JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
					                DEBUG_COLOR_CYAN "%02X "
					                DEBUG_COLOR_DEFAULT,
					                buf[bytes_read]);
					bytes_read++;
				}
				/* for blocking i/o or nonblocking i/o without poll(), */
				/* reading zero bytes is actually OK here. */
				else {
#  ifdef RAWMIDI_USE_POLL
					JAMROUTER_ERROR("Unable to read from Raw MIDI "
					                "device '%s' -- %s!\n",
					                rawmidi->rx_device, strerror(errno));
#  else /* !RAWMIDI_USE_POLL */
					jamrouter_nanosleep(rawmidi_sleep_time);
#  endif /* !RAWMIDI_USE_POLL */
				}
			}
			break;
# endif /* !RAWMIDI_GENERIC_NONBLOCK */
		} /* while() */
		break;
#endif /* ENABLE_RAWMIDI_GENERIC */
	}

	/* return number of midi message bytes read */
	return bytes_read;
}


/******************************************************************************
 * rawmidi_write()
 *      struct rawmidi          rm
 *      char                    *buf
 *      int                     len
 *
 * Writes <len> bytes from <buf> to the rawmidi output device, either
 * one byte or one event (4 bytes) at a time.  Return value is the
 * number of raw midi bytes read, or -1 on error.
 ******************************************************************************/
int
rawmidi_write(RAWMIDI_INFO *rawmidi, unsigned char *buf, ssize_t len)
{
#if defined(ENABLE_RAWMIDI_OSS) || defined(ENABLE_RAWMIDI_OSS2)
	unsigned char       obuf[8];
#endif /* ENABLE_RAWMIDI_OSS */
#ifdef ENABLE_RAWMIDI_OSS
	ssize_t             tx_len          = 0;
#endif /* ENABLE_RAWMIDI_OSS2 */
	ssize_t             j               = 0;
	ssize_t             bytes_written   = 0;

	switch (rawmidi->driver) {

		/* for OSS, send message as raw midi byte events */
#ifdef ENABLE_RAWMIDI_OSS
	case MIDI_DRIVER_RAW_OSS:

		/* raw midi data event type */
		obuf[0] = SEQ_MIDIPUTC;

		/* midi device is always zero */
		obuf[2] = 0;
		//obuf[2] = rawmidi->oss_tx_device;

		/* fourth byte is always zero */
		obuf[3] = 0;

		/* for each byte of raw midi data,
		   fill in data byte of OSS event and send */
		for (j = 0; j < len; j++) {

			/* second byte is the next raw midi byte */
			obuf[1] = buf[j];

			/* write the oss event to raw device */
			if (write(rawmidi->tx_fd, obuf, 4) == 4) {
				JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
				                DEBUG_COLOR_GREEN "%02X " DEBUG_COLOR_DEFAULT,
				                buf[j]);
				if (byte_guard_time_usec > 0) {
					jamrouter_usleep(byte_guard_time_usec);
				}
			}
		}
		bytes_written = j;
		break;
#endif /* ENABLE_RAWMIDI_OSS */

#ifdef ENABLE_RAWMIDI_OSS2
	case MIDI_DRIVER_RAW_OSS2:
		while (tx_len < len) {
			if (buf[0] < 0xA0) {
				obuf[0] = EV_CHN_VOICE;
				obuf[1] = rawmidi->oss_tx_device;
				obuf[2] = buf[tx_len] & 0xF0;
				obuf[3] = buf[tx_len++] & 0x0F;
				obuf[4] = buf[tx_len++];
				obuf[5] = buf[tx_len++];
				obuf[6] = 0;
				obuf[7] = 0;
				tx_len = 3;
			}
			else if ((buf[0] == 0xC0) || (buf[0] == 0xD0)) {
				obuf[0] = EV_CHN_COMMON;
				obuf[1] = rawmidi->oss_tx_device;
				obuf[2] = buf[tx_len] & 0xF0;
				obuf[3] = buf[tx_len++] & 0x0F;
				obuf[4] = 0;
				obuf[5] = 0;
				obuf[6] = buf[tx_len++];
				obuf[7] = 0;
				tx_len = 2;
			}
			else if (buf[0] < 0xF0) {
				obuf[0] = EV_CHN_COMMON;
				obuf[1] = rawmidi->oss_tx_device;
				obuf[2] = buf[tx_len] & 0xF0;
				obuf[3] = buf[tx_len++] & 0x0F;
				obuf[4] = buf[tx_len++];
				obuf[5] = 0;
				obuf[6] = buf[tx_len++];
				obuf[7] = 0;
				tx_len = 3;
			}
			else if (buf[0] == 0xF0) {
				obuf[0] = EV_SYSEX;
				obuf[1] = rawmidi->oss_tx_device;
				for (j = 0; j < 6; j++) {
					obuf[j + 2] = buf[tx_len++];
					if (obuf[j + 2] == 0xF7) {
						break;
					}
				}
			}
			/* write the oss event to raw device */
			if (write(rawmidi->tx_fd, obuf, 8) == 8) {
				JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
				                DEBUG_COLOR_GREEN "%02X %02X %02X %02X %02X %02X %02X %02X "
				                DEBUG_COLOR_DEFAULT,
				                obuf[0], obuf[1], obuf[2], obuf[3],
				                obuf[4], obuf[5], obuf[6], obuf[7]);
				if (byte_guard_time_usec > 0) {
					jamrouter_usleep(byte_guard_time_usec);
				}
			}
			bytes_written += tx_len;
		}
		break;
#endif /* ENABLE_RAWMIDI_OSS2 */

		/* for ALSA, single byte IO seems more dependable */
#ifdef ENABLE_RAWMIDI_ALSA_RAW
	case MIDI_DRIVER_RAW_ALSA:

#ifdef RAWMIDI_ALSA_MULTI_BYTE_IO
		if ((j = snd_rawmidi_write(rawmidi->tx_handle,
		                           buf, (size_t)len)) != len) {
			JAMROUTER_ERROR("Unable to write to ALSA MIDI device '%s'!\n",
			                rawmidi->tx_device);
		}
		if (debug) {
			for (j = 0; j < len; j++) {
				JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
				                DEBUG_COLOR_GREEN "%02X "
				                DEBUG_COLOR_DEFAULT,
				                buf[j]);
			}
		}
#else /* RAWMIDI_ALSA_MULTI_BYTE_IO */
		for (j = 0; j < len; j++) {
			if (snd_rawmidi_write(rawmidi->tx_handle, &buf[j], 1) != 1) {
				JAMROUTER_ERROR("Unable to write to ALSA MIDI device '%s'!\n",
				                rawmidi->tx_device);
				break;
			}
			JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
			                DEBUG_COLOR_GREEN "%02X " DEBUG_COLOR_DEFAULT,
			                buf[j]);
			if (byte_guard_time_usec > 0) {
				jamrouter_usleep(byte_guard_time_usec);
			}
		}
#endif /* RAWMIDI_ALSA_MULTI_BYTE_IO */

		/* A snd_rawmidi_drain() would be nice here. */
		/* The last two attempts were problematic. */
		break;
#endif /* ENABLE_RAWMIDI_ALSA_RAW */

		/* for raw devices, write one byte at a time
		   to make some drivers happy */
#ifdef ENABLE_RAWMIDI_GENERIC
	case MIDI_DRIVER_RAW_GENERIC:
		for (j = 0; j < len; j++) {
			if (write(rawmidi->tx_fd, &buf[j], 1) != 1) {
				JAMROUTER_ERROR("Unable to write to Raw MIDI "
				                "device '%s' -- %s!\n",
				                rawmidi->tx_device, strerror(errno));
				break;
			}
			JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
			                DEBUG_COLOR_GREEN "%02X " DEBUG_COLOR_DEFAULT,
			                buf[j]);
			if (byte_guard_time_usec > 0) {
				jamrouter_usleep(byte_guard_time_usec);
			}
		}
		bytes_written = j;
		break;
#endif /* ENABLE_RAWMIDI_GENERIC */
	}

	return (int)bytes_written;
}


/******************************************************************************
 * rawmidi_flush()
 *  RAWMIDI_INFO *rawmidi
 *
 * Flushes any data waiting to be read from the rawmidi device and
 * simply throws it away.  Nonblocking IO is used to avoid select() or
 * poll() calls, which may not be dependable for some drivers.  Return
 * value is zero on success, or -1 on error.
 ******************************************************************************/
int
rawmidi_flush(RAWMIDI_INFO *rawmidi)
{
#if defined(ENABLE_RAWMIDI_OSS) || defined(ENABLE_RAWMIDI_GENERIC)
# ifdef RAWMIDI_FLUSH_NONBLOCK
	long        flags;
# endif
	char        buf[2];
#endif /* ENABLE_RAWMIDI_OSS || ENABLE_RAWMIDI_GENERIC */

	switch (rawmidi->driver) {
#ifdef ENABLE_RAWMIDI_OSS
	case MIDI_DRIVER_RAW_OSS:
#endif /* ENABLE_RAWMIDI_OSS */
#ifdef ENABLE_RAWMIDI_GENERIC
	case MIDI_DRIVER_RAW_GENERIC:
#endif /* ENABLE_RAWMIDI_RAW_GENERIC */
#if defined(ENABLE_RAWMIDI_OSS) || defined(ENABLE_RAWMIDI_GENERIC)
		/* play tricks with nonblocking io and sync to flush the buffer with
		   standard read(). */

		/* get flags from raw midi descriptor and set to nonblock */
# ifdef RAWMIDI_FLUSH_NONBLOCK
		flags = fcntl(rawmidi->rx_fd, F_GETFL, 0);
		if (fcntl(rawmidi->rx_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
			JAMROUTER_WARN("Unable to set nonblocking IO "
			               "for Raw MIDI device %s:  %s\n",
			            rawmidi->rx_device, strerror(errno));
			return -1;
		}
# endif /* RAWMIDI_FLUSH_NONBLOCK */

		/* For some interfaces, a call to fdatasync() may help ensure that the
		   entire buffer gets flushed.  This has not been known to cause error
		   with interfaces that do not require it.  The same cannot be said
		   for fsync(), which attempts to flush metadata which may not be
		   implemented by the driver. */
		fdatasync(rawmidi->rx_fd);

		/* read raw midi device until there's no data left */
		while (read(rawmidi->rx_fd, buf, 1) == 1) {
			jamrouter_nanosleep(rawmidi_sleep_time);
		}

# ifdef RAWMIDI_FLUSH_NONBLOCK
		/* set original flags for raw midi input descriptor */
		if (fcntl(rawmidi->rx_fd, F_SETFL, flags) != 0) {
			JAMROUTER_WARN("Unable to set blocking IO for raw MIDI -- %s\n",
			            strerror(errno));
			return -1;
		}
# endif /* RAWMIDI_FLUSH_NONBLOCK */

		return 0;
#endif /* ENABLE_RAWMIDI_OSS || ENABLE_RAWMIDI_GENERIC */
#ifdef ENABLE_RAWMIDI_ALSA_RAW
	case MIDI_DRIVER_RAW_ALSA:
# ifdef RAWMIDI_ALSA_NONBLOCK
		while (snd_rawmidi_read(rawmidi->rx_handle, buf, 1) == 1) {
			jamrouter_nanosleep(rawmidi_sleep_time);
		}
# endif /* RAWMIDI_ALSA_NONBLOCK */
		return 0;
#endif /* ENABLE_RAWMIDI_ALSA_RAW */
	}

	return 0;
}


/*****************************************************************************
 * rawmidi_watchdog_cycle()
 *****************************************************************************/
void
rawmidi_watchdog_cycle(void)
{
	ALSA_RAWMIDI_HW_INFO    *cur;
	ALSA_RAWMIDI_HW_INFO    *old_rawmidi_rx_hw;
	ALSA_RAWMIDI_HW_INFO    *new_rawmidi_rx_hw;
	ALSA_RAWMIDI_HW_INFO    *old_rawmidi_tx_hw;
	ALSA_RAWMIDI_HW_INFO    *new_rawmidi_tx_hw;

	if ((midi_driver == MIDI_DRIVER_RAW_ALSA) && (rawmidi_info != NULL)) {

		/* rx */
		old_rawmidi_rx_hw = alsa_rawmidi_rx_hw;
		cur = alsa_rawmidi_rx_hw;
		while (cur != NULL) {
			if (cur->connect_request) {
				cur->connect_request    = 0;
				cur->disconnect_request = 0;
				stop_midi_rx();
				wait_midi_rx_stop();
				rawmidi_init();
				start_midi_rx();
				wait_midi_rx_start();
				break;

			}
			/* Ignore disconnect requests. We can only open 1 at a time. */
			cur = cur->next;
			if (alsa_rawmidi_rx_hw == NULL) {
				break;
			}
		}
		new_rawmidi_rx_hw =
			alsa_rawmidi_get_hw_list(SND_RAWMIDI_STREAM_INPUT);
		if (alsa_rawmidi_hw_list_compare(old_rawmidi_rx_hw,
		                                 new_rawmidi_rx_hw) == 0) {
			alsa_rawmidi_hw_info_free(new_rawmidi_rx_hw);
		}
		else {
			alsa_rawmidi_rx_hw = new_rawmidi_rx_hw;
			alsa_rawmidi_hw_info_free(old_rawmidi_rx_hw);
			alsa_rawmidi_hw_changed = 1;
		}

		/* tx */
		old_rawmidi_tx_hw = alsa_rawmidi_tx_hw;
		cur = alsa_rawmidi_tx_hw;
		while (cur != NULL) {
			if (cur->connect_request) {
				cur->connect_request    = 0;
				cur->disconnect_request = 0;
				stop_midi_tx();
				wait_midi_tx_stop();
				rawmidi_init();
				start_midi_tx();
				wait_midi_tx_start();
				break;

			}
			/* Ignore disconnect requests. We can only open 1 at a time. */
			cur = cur->next;
			if (alsa_rawmidi_tx_hw == NULL) {
				break;
			}
		}
		new_rawmidi_tx_hw =
			alsa_rawmidi_get_hw_list(SND_RAWMIDI_STREAM_OUTPUT);
		if (alsa_rawmidi_hw_list_compare(old_rawmidi_tx_hw,
		                                 new_rawmidi_tx_hw) == 0) {
			alsa_rawmidi_hw_info_free(new_rawmidi_tx_hw);
		}
		else {
			alsa_rawmidi_tx_hw = new_rawmidi_tx_hw;
			alsa_rawmidi_hw_info_free(old_rawmidi_tx_hw);
			alsa_rawmidi_hw_changed = 1;
		}
	}
}


/*****************************************************************************
 * rawmidi_init()
 *
 * Open MIDI device and leave in a ready state for the MIDI thread
 * to start reading events.
 *****************************************************************************/
int
rawmidi_init(void)
{
#ifdef ENABLE_RAWMIDI_ALSA_RAW
	if (midi_driver == MIDI_DRIVER_RAW_ALSA) {
		/* rx */
		if (alsa_rawmidi_rx_hw != NULL) {
			alsa_rawmidi_hw_info_free(alsa_rawmidi_rx_hw);
		}
		alsa_rawmidi_rx_hw =
			alsa_rawmidi_get_hw_list(SND_RAWMIDI_STREAM_INPUT);

		/* tx */
		if (alsa_rawmidi_tx_hw != NULL) {
			alsa_rawmidi_hw_info_free(alsa_rawmidi_tx_hw);
		}
		alsa_rawmidi_tx_hw =
			alsa_rawmidi_get_hw_list(SND_RAWMIDI_STREAM_OUTPUT);
	}
#endif /* ENABLE_RAWMIDI_ALSA_RAW */

	if ((rawmidi_info = rawmidi_open(midi_rx_port_name,
	                                 midi_tx_port_name,
	                                 midi_driver)) == NULL) {
		return -1;
	}

	return 0;
}


/*****************************************************************************
 * rawmidi_cleanup()
 *  void *      arg
 *
 * Cleanup handler for RAWMIDI thread.
 * Closes RAWMIDI ports.
 *****************************************************************************/
void
rawmidi_cleanup(void *arg)
{

	if (arg == (void *)midi_rx_thread_p) {
		midi_rx_thread_p = 0;
		midi_rx_stopped  = 1;
	}
	if (arg == (void *)midi_tx_thread_p) {
		midi_tx_thread_p = 0;
		midi_tx_stopped  = 1;
	}
	if ( (rawmidi_info != NULL) &&
	     (midi_rx_thread_p == 0) && (midi_tx_thread_p == 0) ) {
		rawmidi_close(rawmidi_info);
		rawmidi_free(rawmidi_info);
		rawmidi_info = NULL;
	}

	/* Add some guard time, in case MIDI hardware is re-initialized soon. */
	usleep(125000);
}


/*****************************************************************************
 * rawmidi_read_sysex ()
 *****************************************************************************/
void
rawmidi_read_sysex(volatile MIDI_EVENT *event)
{
	unsigned char   midi_byte;

	do {
		if (rawmidi_read(rawmidi_info, (unsigned char *) &midi_byte, 1) == 1) {
			/*
			  Sysex messages end with 0xF7.  Truncate sysex messages to
			  SYSEX_BUFFER_SIZE in case the terminating byte is either lost in
			  transmission or simply not sent by out of spec devices.
			  SYSEX_BUFFER_SIZE can always be changed, or special handling
			  for obscure sysex messages can be added.  Nonstandard
			  end-sysex bytes are converted to standard 0xF7.
			  TODO: Sysex message timeouts, similar to active sensing.
			*/
			if ( (midi_byte == sysex_terminator) ||
			     (event->bytes == (SYSEX_BUFFER_SIZE - 1)) ) {
				event->data[event->bytes++] = 0xF7;
				if (sysex_extra_terminator == 0xF7) {
					break;
				}
			}
			else if ( (sysex_extra_terminator != 0xF7) &&
			          (event->data[event->bytes - 1] == sysex_terminator) &&
			          (midi_byte == sysex_extra_terminator) ) {
				event->data[event->bytes] = 0xF7;
				break;
			}
			/* handle interleaved MIDI realtime messages */
			else if (midi_byte > 0xF7) {
				JAMROUTER_DEBUG((DEBUG_CLASS_TIMING | DEBUG_CLASS_STREAM),
				                DEBUG_COLOR_CYAN "<%X> " DEBUG_COLOR_DEFAULT,
				                midi_byte);
				midi_realtime_type[realtime_event_count++] = midi_byte;
			}
			/* anything else should be considered part of the sysex message */
			else {
				event->data[event->bytes++] = midi_byte;
			}
		}
		else {
			JAMROUTER_ERROR("*** Raw MIDI Read Error! *** ");
		}
	}
	while (!midi_rx_stopped && !pending_shutdown);
}


/*****************************************************************************
 * rawmidi_read_byte ()
 *
 * Note:  Some devices might break spec and treat all system messages as
 *        realtime messages and interleave sysex and MTC qframes as well.
 *        Special handling for all interleaved system messages should go here.
 *****************************************************************************/
unsigned char
rawmidi_read_byte(volatile MIDI_EVENT *UNUSED(event))
{
	unsigned char   midi_byte;

	if (rawmidi_read(rawmidi_info, (unsigned char *) &midi_byte, 1) == 1) {
		if (midi_byte > 0xF7) {
			JAMROUTER_DEBUG((DEBUG_CLASS_TIMING | DEBUG_CLASS_STREAM),
			                DEBUG_COLOR_CYAN "<%X> " DEBUG_COLOR_DEFAULT,
			                midi_byte);
			midi_realtime_type[realtime_event_count++] = midi_byte;
		}
	}
	else {
		JAMROUTER_ERROR("*** Raw MIDI Read Error! *** ");
	}

	return midi_byte;
}


/*****************************************************************************
 * raw_midi_rx_thread()
 *
 * Raw MIDI input thread function.  Queues incoming MIDI events read from a
 * raw MIDI device.  Since this is raw (not event driven) MIDI input, this
 * thread will properly handle interleaved MIDI realtime events.
 *****************************************************************************/
void *
raw_midi_rx_thread(void *UNUSED(arg))
{
	char                thread_name[16];
	volatile MIDI_EVENT *volatile out_event;
	struct timespec     now;
	struct sched_param  schedparam;
	pthread_t           thread_id;
	unsigned char       running_status;
	unsigned short      event_frame_span    = 0;
	unsigned short      first_byte_frame    = 0;
	unsigned short      last_byte_frame     = 0;
	unsigned short      rx_index            = 0;
	unsigned short      period;
	unsigned short      last_byte_period;
	unsigned char       type                = MIDI_EVENT_NO_EVENT;
	unsigned char       channel             = 0x7F;
	unsigned char       midi_byte;
	unsigned char       j;

	/* with a single reader queue, it is appropriate to ignore */
	/* the declaration of volatile here with a typecast. */
	out_event        = get_new_midi_event(A2J_QUEUE);
	out_event->state = EVENT_STATE_ALLOCATED;

	/* set realtime scheduling and priority */
	thread_id = pthread_self();
	snprintf(thread_name, 16, "jamrouter%c-rx", ('0' + jamrouter_instance));
	pthread_setname_np(thread_id, thread_name);
	memset(&schedparam, 0, sizeof(struct sched_param));
	schedparam.sched_priority = midi_rx_thread_priority;
	pthread_setschedparam(thread_id, JAMROUTER_SCHED_POLICY, &schedparam);

	/* setup thread cleanup handler */
	pthread_cleanup_push(&rawmidi_cleanup, (void *)(thread_id));

	JAMROUTER_DEBUG(DEBUG_CLASS_INIT, "Starting Raw MIDI Rx thread...\n");

	/* flush MIDI input */
#ifdef RAWMIDI_FLUSH_ON_START
	rawmidi_flush(rawmidi_info);
#endif /* RAWMIDI_FLUSH_ON_START */

	/* broadcast the midi ready condition */
	pthread_mutex_lock(&midi_rx_ready_mutex);
	midi_rx_ready = 1;
	pthread_cond_broadcast(&midi_rx_ready_cond);
	pthread_mutex_unlock(&midi_rx_ready_mutex);

	/* MAIN LOOP: read raw midi device and queue events */
	while (!midi_rx_stopped && !pending_shutdown) {

		/* set thread cancelation point */
		pthread_testcancel();

		/* Read new MIDI input, starting with first byte. */
		if (rawmidi_read(rawmidi_info, (unsigned char *) &midi_byte, 1) == 1) {

			period           = get_midi_period(&now);
			first_byte_frame = get_midi_frame(&period, &now,
			                                  FRAME_FIX_LOWER | FRAME_LIMIT_UPPER);
			last_byte_frame  = first_byte_frame;
			rx_index         = sync_info[period].rx_index;

			/* To determine message type, assuming no running status until
			   we learn otherwise. */
			running_status = 0;

			/* No status byte.  Use running status.  Set running_status
			   flag so we don't try to read the second byte again. */
			if (midi_byte < 0x80) {
				running_status = 1;
			}
			/* status byte was found so keep track of message type and
			   channel. */
			else {
				type    = midi_byte & MIDI_TYPE_MASK;     // & 0xF0
				channel = midi_byte & MIDI_CHANNEL_MASK;  // & 0x0F
			}
			/* handle channel events (with or without status byte). */
			if (type < 0xF0) {
				out_event->type    = type;
				out_event->channel = channel;
				/* read second byte if we haven't already */
				if (running_status) {
					out_event->byte2 = midi_byte;
				}
				else {
					out_event->byte2 = rawmidi_read_byte(out_event);
				}
				/* all channel specific messages except program change and
				   polypressure have 2 bytes following status byte */
				switch (type) {
				case MIDI_EVENT_PROGRAM_CHANGE:
				case MIDI_EVENT_POLYPRESSURE:
					out_event->byte3 = 0x00;
					out_event->bytes = 2;
					break;
				case MIDI_EVENT_CONTROLLER:
					out_event->byte3 = rawmidi_read_byte(out_event);
					out_event->bytes = 3;
					break;
				case MIDI_EVENT_NOTE_OFF:
				case MIDI_EVENT_NOTE_ON:
					out_event->byte3 = rawmidi_read_byte(out_event);
					out_event->bytes = 3;
					if ( (out_event->velocity == note_off_velocity) ||
					      (out_event->type == MIDI_EVENT_NOTE_OFF) ) {
						if (rx_queue_real_note_off) {
							out_event->type = MIDI_EVENT_NOTE_OFF;
						}
						out_event->velocity = 0x0;
						track_note_off(A2J_QUEUE,
						               out_event->channel, out_event->note);
					}
					else {
						track_note_on(A2J_QUEUE,
						              out_event->channel, out_event->note);
					}
					break;
				case MIDI_EVENT_AFTERTOUCH:
				case MIDI_EVENT_PITCHBEND:
					out_event->byte3 = rawmidi_read_byte(out_event);
					out_event->bytes = 3;
					break;
				}
			}
			/* handle system and realtime messages */
			else {
				/* clear running status (by setting type) when we see
				   system (but not realtime) messages. */
				/* clock tick and realtime msgs (0xF8 and above) do not
				   clear running status. */
				if (midi_byte < 0xF8) {
					type           = midi_byte;
					channel        = 0x0;
				}
				out_event->type = midi_byte;
				/* switch on midi_byte instead of type, since type could
				   have been masked for channel messages */
				switch (midi_byte) {
					/* variable length system messages */
				case MIDI_EVENT_SYSEX:          // 0xF0
					out_event->data[0] = 0xF0;
					out_event->bytes   = 1;
					rawmidi_read_sysex(out_event);
					break;
					/* 3 byte system messages */
				case MIDI_EVENT_SONGPOS:        // 0xF2
					/* read 2 more bytes (watching out for interleaved
					   realtime msgs). */
					out_event->byte2 = rawmidi_read_byte(out_event);
					out_event->byte3 = rawmidi_read_byte(out_event);
					out_event->bytes = 3;
					break;
					/* 2 byte system messages */
				case MIDI_EVENT_MTC_QFRAME:     // 0xF1
				case MIDI_EVENT_SONG_SELECT:    // 0xF3
					/* read 1 more byte (watching out for interleaved
					   realtime msgs). */
					out_event->byte2 = rawmidi_read_byte(out_event);
					out_event->byte3 = 0x0;
					out_event->bytes = 2;
					break;
					/* 1-byte system and realtime messages */
				case MIDI_EVENT_BUS_SELECT:     // 0xF5
				case MIDI_EVENT_TUNE_REQUEST:   // 0xF6
				case MIDI_EVENT_END_SYSEX:      // 0xF7
				case MIDI_EVENT_TICK:           // 0xF8
				case MIDI_EVENT_START:          // 0xFA
				case MIDI_EVENT_CONTINUE:       // 0xFB
				case MIDI_EVENT_STOP:           // 0xFC
				case MIDI_EVENT_ACTIVE_SENSING: // 0xFE
				case MIDI_EVENT_SYSTEM_RESET:   // 0xFF
					out_event->bytes = 1;
					break;
				default:
					break;
				}
			}

			/* prepare to queue event. */
			if (out_event->bytes > 0) {

				/* keep track of last byte frame and event span for debugging */
				last_byte_period = get_midi_period(&now);
				last_byte_frame  =
					get_midi_frame(&last_byte_period, &now, FRAME_FIX_LOWER);
				event_frame_span = (unsigned short)
					( sync_info[period].buffer_size -
					  (rx_index + first_byte_frame) +
					  sync_info[last_byte_period].rx_index + last_byte_frame )
					& sync_info[period].buffer_size_mask;

				/* Check for events that exceed normal expectations for
				   3-byte events.  With some interfaces, this can be common. */
				if ( event_frame_span > (unsigned short)
				     ( (short)sync_info[period].rx_latency_size -
				       (short)(sync_info[period].buffer_period_size) +
				       (short)midi_phase_min) ) {
					JAMROUTER_DEBUG(DEBUG_CLASS_TESTING,
					                DEBUG_COLOR_RED "?? " DEBUG_COLOR_DEFAULT);
				}
				else if (event_frame_span > (sync_info[period].sample_rate / 1500)) {
					JAMROUTER_DEBUG(DEBUG_CLASS_TESTING,
					                DEBUG_COLOR_RED "? " DEBUG_COLOR_DEFAULT);
				}
#ifndef WITHOUT_JUNO
				/* translate juno sysex to controllers */
				translate_from_juno(period, A2J_QUEUE,
				                    out_event, first_byte_frame, rx_index);
#endif
			}

			/* queue event. */
			if (out_event->bytes > 0) {

				/* queue notes off for all-notes-off controller. */
				if ( (out_event->controller == MIDI_CONTROLLER_ALL_NOTES_OFF) &&
				     (out_event->type       == MIDI_EVENT_CONTROLLER) ) {
					queue_notes_off(period, A2J_QUEUE, out_event->channel,
					                first_byte_frame, rx_index);
				}
				/* otherwise, queue event as is */
				else {
					queue_midi_event(period, A2J_QUEUE, out_event,
					                 first_byte_frame, rx_index, 0);
				}

				JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
				                DEBUG_COLOR_CYAN "[%d-%d:%d] "
				                DEBUG_COLOR_DEFAULT,
				                first_byte_frame, last_byte_frame,
				                event_frame_span);

				out_event = get_new_midi_event(A2J_QUEUE);
			}

			/* Interleaved realtime events can only be queued _after_ the
			   event that the realtime event was interleaved in.  Queuing
			   interleaved events for the same cycle frame as the initial
			   event alleviates this problem completely. */
			for (j = 0; j < realtime_event_count; j++) {
				queue_midi_realtime_event(period, A2J_QUEUE,
				                          midi_realtime_type[j],
				                          first_byte_frame, rx_index);
				midi_realtime_type[j] = 0;
			}
			realtime_event_count = 0;
		} /* if (rawmidi_read()) */

		period = get_midi_period(&now);
		if (check_active_sensing_timeout(period, A2J_QUEUE) > 0) {
			for (j = 0; j < 16; j++) {
				queue_notes_off(period, A2J_QUEUE, j, 0, rx_index);
			}
		}

#ifndef RAWMIDI_USE_POLL
		jamrouter_nanosleep(rawmidi_sleep_time);
#endif /* RAWMIDI_USE_POLL */

	} /* while() */

	/* execute cleanup handler and remove it */
	pthread_cleanup_pop(1);

	/* end of RAWMIDI thread */
	pthread_exit(NULL);
	return NULL;
}


/*****************************************************************************
 * raw_midi_tx_thread()
 *
 * Raw MIDI output thread function.  De-queues incoming MIDI events read to a
 * raw MIDI device.  Since this is raw (not event driven) MIDI input, this
 * thread will properly handle interleaved MIDI realtime events.
 *****************************************************************************/
void *
raw_midi_tx_thread(void *UNUSED(arg))
{
	char                thread_name[16];
	volatile MIDI_EVENT midi_event;
	volatile MIDI_EVENT *event              = &midi_event;
	volatile MIDI_EVENT *next               = NULL;
	volatile MIDI_EVENT *cur;
	struct timespec     now;
	struct sched_param  schedparam;
	pthread_t           thread_id;
#ifdef ENABLE_DEBUG
	short               event_latency;
	unsigned short      end_frame;
	unsigned short      end_period;
#endif
	unsigned short      cycle_frame;
	unsigned short      period;
	unsigned short      last_period;
	unsigned short      all_notes_off       = 0;
	unsigned char       running_status      = 0xFF;
	unsigned char       last_running_status = 0xFF;
	unsigned char       first;
	unsigned char       sleep_once;

	event->state = EVENT_STATE_ALLOCATED;

	/* set realtime scheduling and priority */
	thread_id = pthread_self();
	snprintf(thread_name, 16, "jamrouter%c-tx", ('0' + jamrouter_instance));
	pthread_setname_np(thread_id, thread_name);
	memset(&schedparam, 0, sizeof(struct sched_param));
	schedparam.sched_priority = midi_tx_thread_priority;
	pthread_setschedparam(thread_id, JAMROUTER_SCHED_POLICY, &schedparam);

	/* setup thread cleanup handler */
	pthread_cleanup_push(&rawmidi_cleanup, (void *)(thread_id));

	JAMROUTER_DEBUG(DEBUG_CLASS_INIT, "Starting Raw MIDI Tx thread...\n");

	/* drain MIDI output */
#ifdef RAWMIDI_FLUSH_ON_START
	//rawmidi_drain(rawmidi_info);
#endif /* RAWMIDI_FLUSH_ON_START */

	/* broadcast the midi ready condition */
	pthread_mutex_lock(&midi_tx_ready_mutex);
	midi_tx_ready = 1;
	pthread_cond_broadcast(&midi_tx_ready_cond);
	pthread_mutex_unlock(&midi_tx_ready_mutex);

	period = get_midi_period(&now);
	period = sleep_until_next_period(period, &now);
	cycle_frame = sync_info[period].buffer_period_size;

	/* MAIN LOOP: read raw midi device and queue events */
	while (!midi_tx_stopped && !pending_shutdown) {

		/* set thread cancelation point */
		pthread_testcancel();

		if (cycle_frame >= sync_info[period].buffer_period_size) {
			cycle_frame = 0;

			/* sleep (if necessary) until next midi period has started. */
			last_period = period;
			period = sleep_until_next_period(period, &now);
		}

		event = dequeue_midi_event(J2A_QUEUE, &last_period, period, cycle_frame);

		/* Look ahead for optional translation of note on/off events */
		if ( note_on_velocity || note_off_velocity ||
		     tx_prefer_real_note_off || tx_prefer_all_notes_off ) {
			all_notes_off = 0;
			cur = event;
			while ((cur != NULL) && (cur->state == EVENT_STATE_QUEUED)) {
				if (cur->type == MIDI_EVENT_NOTE_ON) {
					if (cur->velocity == 0) {
						if (tx_prefer_real_note_off) {
							cur->type = MIDI_EVENT_NOTE_OFF;
						}
						if (note_off_velocity != 0x0) {
							cur->velocity = note_off_velocity;
						}
					}
					else if (note_on_velocity != 0x0) {
						cur->velocity = note_on_velocity;
					}
				}
				else if ( (cur->type == MIDI_EVENT_NOTE_OFF) &&
				          (note_off_velocity != 0x0) ) {
					cur->velocity = note_off_velocity;
				}
				else if ( tx_prefer_all_notes_off &&
				          (cur->type == MIDI_EVENT_CONTROLLER) &&
				          (cur->controller == MIDI_CONTROLLER_ALL_NOTES_OFF) ) {
					all_notes_off |=
						(unsigned short)(1 << (cur->channel & 0x0F));
				}
				cur = cur->next;
			}
		}

		first = 1;
		while ((event != NULL) && (event->state == EVENT_STATE_QUEUED)) {
			if (first) {
				JAMROUTER_DEBUG(DEBUG_CLASS_TX_TIMING,
				                DEBUG_COLOR_YELLOW ": " DEBUG_COLOR_DEFAULT);
			}
			first = 0;

			/* ignore note-off message for any channels 
			   with all-notes-off messages. */
			if ( (all_notes_off & (1 << (event->channel & 0x0F))) &&
			     (event->type == MIDI_EVENT_NOTE_ON) &&
			     (event->velocity == note_off_velocity) ) {
				event->bytes = 0;
				JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
				                DEBUG_COLOR_GREEN "-----%X:%02X----- "
				                DEBUG_COLOR_DEFAULT,
				                event->channel, event->note);
			}

			if (event->bytes > 0) {
				/* handle messages with channel number embedded in the first byte */
				if (event->type < 0xF0) {
					running_status = (unsigned char)((event->type & 0xF0) |
					                                 (event->channel & 0x0F));
					tx_buf[0] = running_status;
					tx_buf[1] = (unsigned char)event->byte2;
					/* all channel specific messages except program change and
					   polypressure have 2 bytes following status byte */
					if ( (event->type == MIDI_EVENT_PROGRAM_CHANGE) ||
					     (event->type == MIDI_EVENT_POLYPRESSURE)      ) {
						tx_buf[2] = (unsigned char)0x0;
					}
					else {
						tx_buf[2] = (unsigned char)event->byte3;
					}
					/* internal MIDI resync event not needed for JAMRouter's
					   current design, but may be useful in the future. */
					//if (event->type == MIDI_EVENT_RESYNC) {
					//	event->bytes                 = 0;
					//	JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
					//	                DEBUG_COLOR_GREEN "<<<<<SYNC>>>>> "
					//                  DEBUG_COLOR_DEFAULT);
					//}
				}
				/* handle system (non-channel) messages */
				else {
					tx_buf[0] = (unsigned char)(event->type);
					switch (event->type) {
					case MIDI_EVENT_SYSEX:          // 0xF0
						memcpy(tx_buf, (void *)(event->data), event->bytes);
						running_status = 0xFF;
						break;
						/* 3 byte system messages */
					case MIDI_EVENT_SONGPOS:        // 0xF2
						tx_buf[1] = (unsigned char)event->byte2;
						tx_buf[2] = (unsigned char)event->byte3;
						running_status = 0xFF;
						break;
						/* 2 byte system messages */
					case MIDI_EVENT_MTC_QFRAME:     // 0xF1
					case MIDI_EVENT_SONG_SELECT:    // 0xF3
						tx_buf[1] = (unsigned char)event->byte2;
						running_status = 0xFF;
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
						event->bytes = 0;
						JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
						                DEBUG_COLOR_GREEN ">%02X< "
						                DEBUG_COLOR_DEFAULT,
						                event->type);
						break;
					}
				}

				/* Handle event if it has bytes that need to be written */
				if (event->bytes > 0) {

					if (sleep_once) {
						sleep_until_frame(period, cycle_frame);
						sleep_once = 0;
					}
#ifdef ENABLE_DEBUG
					end_period = get_midi_period(&now);
					end_frame = get_midi_frame(&end_period, &now, FRAME_FIX_LOWER);
#endif
					/* Write the event to MIDI hardware */
					if ( use_running_status &&
					     (tx_buf[0] == last_running_status) ) {
						rawmidi_write(rawmidi_info, &(tx_buf[1]),
						              (ssize_t)(event->bytes) - 1);
					}
					else {
						rawmidi_write(rawmidi_info, tx_buf,
						              (ssize_t)(event->bytes));
						last_running_status = running_status;
					}
#ifdef ENABLE_DEBUG
					event_latency = (short)
						( ( (sync_info[period].buffer_size + 
						     sync_info[end_period].tx_index + end_frame) -
						    (sync_info[period].tx_index + cycle_frame) )
						  & sync_info[period].buffer_size_mask );

					JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
					                DEBUG_COLOR_GREEN "[%d%+d] "
					                DEBUG_COLOR_DEFAULT,
					                cycle_frame, event_latency);
#endif
					/* optional Tx guard interval between messages */
					if (event_guard_time_usec > 0) {
						jamrouter_usleep(event_guard_time_usec);
					}
				}
			}

			/* keep track of next event */
			next = event->next;

			/* Clear event. */
			event->type    = 0;
			event->channel = 0;
			event->byte2   = 0;
			event->byte3   = 0;
			event->next    = NULL;
			event->state   = EVENT_STATE_FREE;

			/* ready to process next event */
			event = next;
		} /* while() */
		cycle_frame++;
		sleep_once = 1;
	} /* while () */

	/* execute cleanup handler and remove it */
	pthread_cleanup_pop(1);

	/* end of RAWMIDI Tx thread */
	pthread_exit(NULL);
	return NULL;
}
