/*****************************************************************************
 *
 * alsa_seq.c
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
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <asoundlib.h>
#include "jamrouter.h"
#include "timekeeping.h"
#include "alsa_seq.h"
#include "mididefs.h"
#include "midi_event.h"
#include "driver.h"
#include "debug.h"

#ifndef WITHOUT_LASH
# include "lash.h"
#endif
#ifndef WITHOUT_JUNO
#include "juno.h"
#endif


ALSA_SEQ_INFO   *alsa_seq_info;

int             alsa_seq_ports_changed      = 0;
int             alsa_seq_sleep_time         = 100000;


/*****************************************************************************
 * alsa_error_handler()
 *
 * Placeholder for a real error handling function.  Please note that this
 * handles errors for all of ALSA lib, not just ALSA seq.
 *****************************************************************************/
void
alsa_error_handler(const char   *file,
                   int          line,
                   const char   *func,
                   int          err,
                   const char   *fmt, ...)
{
	JAMROUTER_ERROR("Unhandled ALSA error %d "
	                "in function %s from file %s line %d:\n",
	                err, func, file, line);
	JAMROUTER_ERROR("%s", fmt);
}


/*****************************************************************************
 * alsa_seq_port_free()
 *****************************************************************************/
void
alsa_seq_port_free(ALSA_SEQ_PORT *portinfo)
{
	ALSA_SEQ_PORT   *cur = portinfo;
	ALSA_SEQ_PORT   *next;

	while (cur != NULL) {
		if (cur->client_name != NULL) {
			free(cur->client_name);
		}
		if (cur->port_name != NULL) {
			free(cur->port_name);
		}
		if (cur->subs != NULL) {
			snd_seq_port_subscribe_free(cur->subs);
		}
		next = cur->next;
		free(cur);
		cur = next;
	}
}


/*****************************************************************************
 * alsa_seq_port_list_compare()
 *****************************************************************************/
int
alsa_seq_port_list_compare(ALSA_SEQ_PORT *a, ALSA_SEQ_PORT *b)
{
	ALSA_SEQ_PORT   *cur_a = a;
	ALSA_SEQ_PORT   *cur_b = b;

	while (cur_a != NULL) {
		if ((cur_b == NULL) ||
		    (cur_a->client != cur_b->client) ||
		    (cur_a->port != cur_b->port) ||
		    (strcmp(cur_a->client_name, cur_b->client_name) != 0)) {
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


/*****************************************************************************
 * alsa_seq_port_in_list()
 *****************************************************************************/
int
alsa_seq_port_in_list(int client, int port, ALSA_SEQ_PORT *port_list)
{
	ALSA_SEQ_PORT   *cur = port_list;

	while (cur != NULL) {
		if ((client == cur->client) && (port == cur->port)) {
			return 1;
		}
		cur = cur->next;
	}

	return 0;
}


/*****************************************************************************
 * alsa_seq_get_port_list()
 *****************************************************************************/
ALSA_SEQ_PORT *
alsa_seq_get_port_list(ALSA_SEQ_INFO    *seq_info,
                       unsigned int     caps,
                       ALSA_SEQ_PORT    *list)
{
	ALSA_SEQ_PORT           *head  = list;
	ALSA_SEQ_PORT           *check = list;
	ALSA_SEQ_PORT           *prev  = NULL;
	ALSA_SEQ_PORT           *cur   = NULL;
	ALSA_SEQ_PORT           *seq_port;
	snd_seq_client_info_t   *cinfo;
	snd_seq_port_info_t     *pinfo;
	unsigned int            type;
	int                     client;

	snd_seq_client_info_alloca(&cinfo);
	snd_seq_port_info_alloca(&pinfo);

	snd_seq_client_info_set_client(cinfo, -1);
	while (snd_seq_query_next_client(seq_info->seq, cinfo) >= 0) {
		client = snd_seq_client_info_get_client(cinfo);

		snd_seq_port_info_set_client(pinfo, client);
		snd_seq_port_info_set_port(pinfo, -1);
		while (snd_seq_query_next_port(seq_info->seq, pinfo) >= 0) {
			type = snd_seq_port_info_get_type(pinfo);
			if ((snd_seq_port_info_get_capability(pinfo) & caps) != caps) {
				continue;
			}
			if (alsa_seq_port_in_list(snd_seq_port_info_get_client(pinfo),
			                          snd_seq_port_info_get_port(pinfo), list)) {
				continue;
			}
			if ((seq_port = malloc(sizeof(ALSA_SEQ_PORT))) == NULL) {
				jamrouter_shutdown("Out of memory!\n");
			}
			seq_port->type        = type;
			seq_port->client      = snd_seq_port_info_get_client(pinfo);
			seq_port->port        = snd_seq_port_info_get_port(pinfo);
			seq_port->client_name = strdup(snd_seq_client_info_get_name(cinfo));
			seq_port->port_name   = strdup(snd_seq_port_info_get_name(pinfo));
			seq_port->addr        = snd_seq_port_info_get_addr(pinfo);
			snprintf(seq_port->alsa_name,
			         sizeof(seq_port->alsa_name),
			         "%d:%d",
			         seq_port->client,
			         seq_port->port);
			seq_port->subs = NULL;
			seq_port->next = NULL;
			seq_port->subscribe_request   = 0;
			seq_port->unsubscribe_request = 0;
			if (head == NULL) {
				head = check = cur = seq_port;
			}
			else {
				while (check != NULL) {
					if ((check->client > seq_port->client) ||
					    ((check->client == seq_port->client) &&
					     (check->port > seq_port->port))) {
						seq_port->next = check;
						if (prev == NULL) {
							seq_port->next = head;
							head = prev = seq_port;
						}
						else {
							prev->next = seq_port;
						}
						prev = seq_port;
						break;
					}
					else {
						prev = check;
						check = check->next;
					}
				}
				if (check == NULL) {
					cur->next = seq_port;
					cur = seq_port;
				}
			}
		}
	}

	return head;
}


/*****************************************************************************
 * alsa_seq_subscribe_port()
 *****************************************************************************/
void
alsa_seq_subscribe_port(ALSA_SEQ_INFO   *seq_info,
                        ALSA_SEQ_PORT   *seq_port,
                        char            *port_str_list,
                        int             tx)
{
	snd_seq_addr_t              sender;
	snd_seq_addr_t              dest;
	snd_seq_port_subscribe_t    *subs;

	if (seq_port->subs == NULL) {
		snd_seq_port_subscribe_malloc(&subs);

		if (tx) {
			dest.client   = (unsigned char)(seq_port->client          & 0xFF);
			dest.port     = (unsigned char)(seq_port->port            & 0xFF);
			sender.client = (unsigned char)(seq_info->tx_port->client & 0xFF);
			sender.port   = (unsigned char)(seq_info->tx_port->port   & 0xFF);
		}
		else {
			sender.client = (unsigned char)(seq_port->client          & 0xFF);
			sender.port   = (unsigned char)(seq_port->port            & 0xFF);
			dest.client   = (unsigned char)(seq_info->rx_port->client & 0xFF);
			dest.port     = (unsigned char)(seq_info->rx_port->port   & 0xFF);
		}

		snd_seq_port_subscribe_set_sender(subs, &sender);
		snd_seq_port_subscribe_set_dest(subs, &dest);
		snd_seq_subscribe_port(seq_info->seq, subs);

		seq_port->subs = subs;
		seq_port->subscribe_request   = 0;
		seq_port->unsubscribe_request = 0;

		if (port_str_list != NULL) {
			strcat(port_str_list, seq_port->alsa_name);
			strcat(port_str_list, ",");
		}
		JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER,
		                "  Jamrouter %s subscribed to [%s] %s: %s\n",
		                tx ? "Tx" : "Rx",
		                seq_port->alsa_name,
		                seq_port->client_name,
		                seq_port->port_name);
	}
}


/*****************************************************************************
 * alsa_seq_unsubscribe_port()
 *****************************************************************************/
void
alsa_seq_unsubscribe_port(ALSA_SEQ_INFO *seq_info, ALSA_SEQ_PORT *seq_port)
{
	if (seq_port->subs != NULL) {
		snd_seq_unsubscribe_port(seq_info->seq, seq_port->subs);
		snd_seq_port_subscribe_free(seq_port->subs);

		seq_port->subs = NULL;
		seq_port->subscribe_request   = 0;
		seq_port->unsubscribe_request = 0;

		JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER,
		                "  Unsubscribed from [%s] %s: %s\n",
		                seq_port->alsa_name,
		                seq_port->client_name,
		                seq_port->port_name);
	}
}


/*****************************************************************************
 * alsa_seq_subscribe_ports()
 *   ALSA_SEQ_INFO *seq_info    collected information about our seq client.
 *   ALSA_SEQ_PORT *port_list   list of available subscription ports
 *   unsigned int type          type mask for sequencer port  (0 == no mask)
 *   char *port_str_list        buffer for building list (NULL to disable).
 *   char *port_name_match      substring match for port name (NULL == no match)
 *****************************************************************************/
void
alsa_seq_subscribe_ports(ALSA_SEQ_INFO  *seq_info,
                         ALSA_SEQ_PORT  *seq_port_list,
                         unsigned int   type,
                         char           *port_str_list,
                         char           *port_name_match,
                         ALSA_SEQ_PORT  *selection_list,
                         int            tx)
{
	char                *alsa_name_match = NULL;
	ALSA_SEQ_PORT       *cur             = seq_port_list;
	ALSA_SEQ_PORT       *check;
	int                 port_match_found;

	while (cur != NULL) {

		/* if port match is given, ignore port names that don't match */
		if ((port_name_match != NULL) &&
		    (strstr(cur->port_name, port_name_match) == NULL)) {
			cur = cur->next;
			continue;
		}

		/* if another port list is given to select from, check it for a match */
		check = selection_list;
		port_match_found = 0;
		while (check != NULL) {
			if ((cur->client == check->client) && (cur->port == check->port)) {
				port_match_found = 1;
			}
			check = check->next;
		}
		if ((selection_list != NULL) && !port_match_found) {
			cur = cur->next;
			continue;
		}

		/* if alsa_name match is given, ignore port names that don't match */
		if ((alsa_name_match != NULL) &&
		    (strstr(cur->alsa_name, alsa_name_match) == NULL)) {
			cur = cur->next;
			continue;
		}

		/* check for valid client and type match */
		if ((cur->client >= 0) &&
		    (cur->client != SND_SEQ_ADDRESS_SUBSCRIBERS) &&
		    ((cur->type & type) == type)) {

			alsa_seq_subscribe_port(seq_info, cur, port_str_list, tx);
		}

		cur = cur->next;
	}
}


/*****************************************************************************
 * alsa_seq_watchdog_cycle()
 *****************************************************************************/
void
alsa_seq_watchdog_cycle(void)
{
	ALSA_SEQ_PORT   *cur;
	ALSA_SEQ_PORT   *old_capture_ports;
	ALSA_SEQ_PORT   *new_capture_ports;
	ALSA_SEQ_PORT   *old_playback_ports;
	ALSA_SEQ_PORT   *new_playback_ports;

	if ((midi_driver == MIDI_DRIVER_ALSA_SEQ) && (alsa_seq_info != NULL)) {

		/* rx */
		old_capture_ports = alsa_seq_info->capture_ports;
		cur = alsa_seq_info->capture_ports;
		while (cur != NULL) {
			if (cur->subscribe_request) {
				alsa_seq_subscribe_port(alsa_seq_info, cur, NULL, 0);
			}
			else if (cur->unsubscribe_request) {
				alsa_seq_unsubscribe_port(alsa_seq_info, cur);
			}
			cur = cur->next;
		}
		new_capture_ports =
			alsa_seq_get_port_list(alsa_seq_info,
			                       (SND_SEQ_PORT_CAP_READ |
			                        SND_SEQ_PORT_CAP_SUBS_READ),
			                       NULL);
		if (alsa_seq_port_list_compare(old_capture_ports,
		                               new_capture_ports) == 0) {
			alsa_seq_port_free(new_capture_ports);
		}
		else {
			alsa_seq_info->capture_ports = new_capture_ports;
			alsa_seq_port_free(old_capture_ports);
			alsa_seq_ports_changed = 1;
		}

		/* tx */
		old_playback_ports = alsa_seq_info->playback_ports;
		cur = alsa_seq_info->playback_ports;
		while (cur != NULL) {
			if (cur->subscribe_request) {
				alsa_seq_subscribe_port(alsa_seq_info, cur, NULL, 1);
			}
			else if (cur->unsubscribe_request) {
				alsa_seq_unsubscribe_port(alsa_seq_info, cur);
			}
			cur = cur->next;
		}
		new_playback_ports =
			alsa_seq_get_port_list(alsa_seq_info,
			                       (SND_SEQ_PORT_CAP_WRITE |
			                        SND_SEQ_PORT_CAP_SUBS_WRITE),
			                       NULL);
		if (alsa_seq_port_list_compare(old_playback_ports,
		                               new_playback_ports) == 0) {
			alsa_seq_port_free(new_playback_ports);
		}
		else {
			alsa_seq_info->playback_ports = new_playback_ports;
			alsa_seq_port_free(old_playback_ports);
			alsa_seq_ports_changed = 1;
		}
	}
}


/*****************************************************************************
 * get_alsa_seq_ports()
 *****************************************************************************/
ALSA_SEQ_PORT *
get_alsa_seq_ports(ALSA_SEQ_INFO *seq_info, char *alsa_ports)
{
	ALSA_SEQ_PORT           *head               = NULL;
	ALSA_SEQ_PORT           *cur                = NULL;
	ALSA_SEQ_PORT           *prev               = NULL;
	snd_seq_client_info_t   *cinfo;
	snd_seq_port_info_t     *pinfo;
	char                    *o;
	char                    *p;
	char                    *q;
	char                    *tokbuf;
	int                     client              = 0;
	int                     port                = 0;

	snd_seq_client_info_alloca(&cinfo);
	snd_seq_port_info_alloca(&pinfo);

	/* get a comma separated client:port list from command line, */
	/*  or a '-' (or no -p arg) for open subscription. */
	if (alsa_ports != NULL) {
		if ((tokbuf = alloca(strlen((const char *) alsa_ports) * 4)) == NULL) {
			jamrouter_shutdown("Out of memory!\n");
		}
		o = alsa_ports;
		while ((p = strtok_r(o, ",", &tokbuf)) != NULL) {
			o = NULL;
			prev = cur;
			if (*p == '-') {
				continue;
			}
			else if (isdigit(*p)) {
				if ((q = index(p, ':')) == NULL) {
					JAMROUTER_ERROR("Invalid ALSA MIDI client port '%s'.\n",
					                alsa_ports);
					continue;
				}
				client = atoi(p);
				port   = atoi(q + 1);
			}
			else if (strcmp(p, "autohw") == 0) {
				seq_info->auto_hw = 1;
				continue;
			}
			else if (strcmp(p, "autosw") == 0) {
				seq_info->auto_sw = 1;
				continue;
			}
			else {
				JAMROUTER_ERROR("Invalid ALSA Sequencer client port '%s' in '%s'.\n",
				                p, alsa_ports);
				continue;
			}
			if ((cur = malloc(sizeof(ALSA_SEQ_PORT))) == NULL) {
				jamrouter_shutdown("Out of memory!\n");
			}
			snd_seq_get_any_client_info(seq_info->seq, client, cinfo);
			snd_seq_get_any_port_info(seq_info->seq, client, port, pinfo);
			cur->client      = client;
			cur->port        = port;
			cur->type        = SND_SEQ_PORT_TYPE_MIDI_GENERIC;
			cur->next        = NULL;
			cur->subs        = NULL;
			cur->client_name = strdup(snd_seq_client_info_get_name(cinfo));
			cur->port_name   = strdup(snd_seq_port_info_get_name(pinfo));
			snprintf(cur->alsa_name, sizeof(cur->alsa_name), "%d:%d",
			         cur->client, cur->port);
			if (prev == NULL) {
				head = cur;
			}
			else {
				prev->next = cur;
			}
		}
	}

	return head;
}


/*****************************************************************************
 * open_alsa_seq()
 *
 * Creates MIDI output port and connects specified MIDI input ports to it.
 *****************************************************************************/
ALSA_SEQ_INFO *
open_alsa_seq(char *alsa_capture_ports, char *alsa_playback_ports)
{
	char                    capture_port_str_list[128]  = "\0";
	char                    playback_port_str_list[128] = "\0";
	char                    client_name[32];
	char                    port_name[32];
	ALSA_SEQ_INFO           *new_seq_info;

	/* allocate our MIDI structure for returning everything */
	if ((new_seq_info = malloc(sizeof(ALSA_SEQ_INFO))) == NULL) {
		jamrouter_shutdown("Out of memory!\n");
	}
	memset(new_seq_info, 0, sizeof(ALSA_SEQ_INFO));
	if ((new_seq_info->rx_port = malloc(sizeof(ALSA_SEQ_PORT))) == NULL) {
		jamrouter_shutdown("Out of memory!\n");
	}
	memset(new_seq_info->rx_port, 0, sizeof(ALSA_SEQ_PORT));
	if ((new_seq_info->tx_port = malloc(sizeof(ALSA_SEQ_PORT))) == NULL) {
		jamrouter_shutdown("Out of memory!\n");
	}
	memset(new_seq_info->tx_port, 0, sizeof(ALSA_SEQ_PORT));
	new_seq_info->rx_port->next  = NULL;
	new_seq_info->rx_port->subs  = NULL;
	new_seq_info->tx_port->next  = NULL;
	new_seq_info->tx_port->subs  = NULL;
	new_seq_info->src_ports      = NULL;
	new_seq_info->dest_ports     = NULL;

	/* TODO: need a better handler here */
	snd_lib_error_set_handler(alsa_error_handler);

	/* open the sequencer */
	if (snd_seq_open(&new_seq_info->seq, "default",
	                 SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
		JAMROUTER_ERROR("Unable to open ALSA sequencer.\n");
		return NULL;
	}

	snd_midi_event_new(SYSEX_BUFFER_SIZE, &(new_seq_info->encoder));

	/* get ports based on comma separated client:port lists. */
	new_seq_info->src_ports = get_alsa_seq_ports(new_seq_info, alsa_capture_ports);
	new_seq_info->dest_ports = get_alsa_seq_ports(new_seq_info, alsa_playback_ports);

	/* extract our client id for out ports */
	new_seq_info->rx_port->client = snd_seq_client_id(new_seq_info->seq);
	new_seq_info->tx_port->client = snd_seq_client_id(new_seq_info->seq);

	/* set our client name */
	snprintf(client_name, sizeof(client_name) - 1,
	         "jamrouter%c", jamrouter_instance > 1 ? ('0' + jamrouter_instance) : '\0');
	if (snd_seq_set_client_name(new_seq_info->seq, client_name) < 0) {
		JAMROUTER_ERROR("Unable to set ALSA sequencer client name.\n");
		alsa_seq_cleanup(NULL);
		return NULL;
	}

	/* create rx and tx ports */
	snprintf(port_name, sizeof(port_name), "midi_rx");
	new_seq_info->rx_port->port =
		snd_seq_create_simple_port(new_seq_info->seq, port_name,
		                           SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
		                           SND_SEQ_PORT_TYPE_SOFTWARE |
		                           SND_SEQ_PORT_TYPE_MIDI_GENERIC);

	snprintf(port_name, sizeof(port_name), "midi_tx");
	new_seq_info->tx_port->port =
		snd_seq_create_simple_port(new_seq_info->seq, port_name,
		                           SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
		                           SND_SEQ_PORT_TYPE_SOFTWARE |
		                           SND_SEQ_PORT_TYPE_MIDI_GENERIC);

	/* since we opened nonblocking, we need our poll descriptors */
	if ((new_seq_info->npfds = snd_seq_poll_descriptors_count
	     (new_seq_info->seq, POLLIN)) > 0) {
		if ((new_seq_info->pfds = malloc((unsigned int)(new_seq_info->npfds) *
		                                sizeof(struct pollfd))) == NULL) {
			jamrouter_shutdown("Out of memory!\n");
		}
		if (snd_seq_poll_descriptors(new_seq_info->seq, new_seq_info->pfds,
		                             (unsigned int) new_seq_info->npfds, POLLIN) <= 0) {
			JAMROUTER_ERROR("No ALSA sequencer descriptors to poll.\n");
			alsa_seq_cleanup(NULL);
			return NULL;
		}
	}

	/* get list of all capture ports  */
	if ((new_seq_info->capture_ports =
	     alsa_seq_get_port_list(new_seq_info,
	                            (SND_SEQ_PORT_CAP_READ |
	                             SND_SEQ_PORT_CAP_SUBS_READ),
	                            new_seq_info->capture_ports)) == NULL) {
		JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER, "Unable to get ALSA sequencer port list.\n");
	}

	/* subscribe to ports if any capture ports are available */
	if (new_seq_info->capture_ports != NULL) {
		/* subscribe to hardware ports. */
		if (new_seq_info->auto_hw) {
			JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER,
			             "Auto-subscribing to ALSA sequencer hardware capture ports:\n");
			alsa_seq_subscribe_ports(new_seq_info,
			                         new_seq_info->capture_ports,
			                         SND_SEQ_PORT_TYPE_HARDWARE,
			                         NULL, NULL, NULL, 0);
			strcat(capture_port_str_list, "autohw,");
			JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER, "\n");
		}
		/* subscribe to specified ports */
		if (new_seq_info->src_ports != NULL) {
			JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER,
			             "Subscribing to user specified ALSA sequencer capture ports:\n");
			alsa_seq_subscribe_ports(new_seq_info,
			                         new_seq_info->capture_ports,
			                         0,
			                         capture_port_str_list,
			                         NULL,
			                         new_seq_info->src_ports,
			                         0);
			JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER, "\n");
		}
	}


	/* get list of all playback ports  */
	if ((new_seq_info->playback_ports =
	     alsa_seq_get_port_list(new_seq_info,
	                            (SND_SEQ_PORT_CAP_WRITE |
	                             SND_SEQ_PORT_CAP_SUBS_WRITE),
	                            new_seq_info->playback_ports)) == NULL) {
		JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER, "Unable to get ALSA sequencer playback port list.\n");
	}

	/* subscribe to ports if any playback ports are available */
	if (new_seq_info->playback_ports != NULL) {
		/* subscribe to hardware ports. */
		if (new_seq_info->auto_hw) {
			JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER,
			             "Auto-subscribing to ALSA sequencer hardware playback ports:\n");
			alsa_seq_subscribe_ports(new_seq_info,
			                         new_seq_info->playback_ports,
			                         SND_SEQ_PORT_TYPE_HARDWARE,
			                         NULL, NULL, NULL, 1);
			strcat(playback_port_str_list, "autohw,");
			JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER, "\n");
		}
		/* subscribe to specified ports */
		if (new_seq_info->src_ports != NULL) {
			JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER,
			             "Subscribing to user specified ALSA sequencer playback ports:\n");
			alsa_seq_subscribe_ports(new_seq_info,
			                         new_seq_info->playback_ports,
			                         0,
			                         playback_port_str_list,
			                         NULL,
			                         new_seq_info->dest_ports,
			                         1);
			JAMROUTER_DEBUG(DEBUG_CLASS_DRIVER, "\n");
		}
	}

	if (capture_port_str_list[0] != '\0') {
		* (rindex(capture_port_str_list, ',')) = '\0';
	}
	if (playback_port_str_list[0] != '\0') {
		* (rindex(playback_port_str_list, ',')) = '\0';
	}

	/* ALSA sequencer interface is up and running. */
	return new_seq_info;
}


/*****************************************************************************
 * alsa_seq_init()
 *****************************************************************************/
int
alsa_seq_init(void)
{
	JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
	                "alsa_seq_init() rx_port_name='%s' tx_port_name='%s'\n",
	                midi_rx_port_name, midi_tx_port_name);

	/* open ALSA sequencer MIDI input (OK of port_name is NULL). */
	if ((alsa_seq_info = open_alsa_seq(midi_rx_port_name, midi_tx_port_name)) == NULL) {
		return -1;
	}

#ifndef WITHOUT_LASH
	if (!lash_disabled) {
		lash_client_set_alsa_id(alsa_seq_info->seq);
	}
#endif

	return 0;
}


/*****************************************************************************
 * alsa_seq_cleanup()
 *
 * Cleanup handler for MIDI thread.
 * Closes MIDI ports.
 *****************************************************************************/
void
alsa_seq_cleanup(void *arg)
{
	ALSA_SEQ_PORT   *cur;
	ALSA_SEQ_PORT   *prev;

	/* disconnect from list of specified source ports, if any */
	if (alsa_seq_info != NULL) {

		/* rx */
		if (arg == (void *)midi_rx_thread_p) {
			cur = alsa_seq_info->src_ports;
			while (cur != NULL) {
				if (cur->subs != NULL) {
					snd_seq_unsubscribe_port(alsa_seq_info->seq, cur->subs);
					snd_seq_port_subscribe_free(cur->subs);
				}
				if ((cur->client >= 0) && (cur->client != SND_SEQ_ADDRESS_SUBSCRIBERS)) {
					snd_seq_disconnect_from(alsa_seq_info->seq, 0, cur->client, cur->port);
				}
				if (cur->client_name != NULL) {
					free(cur->client_name);
				}
				if (cur->port_name != NULL) {
					free(cur->port_name);
				}
				prev = cur;
				cur = cur->next;
				free(prev);
			}
			cur = alsa_seq_info->rx_port;
			while (cur != NULL) {
				if (cur->client_name != NULL) {
					free(cur->client_name);
				}
				if (cur->port_name != NULL) {
					free(cur->port_name);
				}
				prev = cur;
				cur = cur->next;
				free(prev);
			}
			midi_rx_thread_p = 0;
			midi_rx_stopped = 1;
		}

		/* tx */
		if (arg == (void *)midi_tx_thread_p) {
			cur = alsa_seq_info->dest_ports;
			while (cur != NULL) {
				if (cur->subs != NULL) {
					snd_seq_unsubscribe_port(alsa_seq_info->seq, cur->subs);
					snd_seq_port_subscribe_free(cur->subs);
				}
				if ((cur->client >= 0) && (cur->client != SND_SEQ_ADDRESS_SUBSCRIBERS)) {
					snd_seq_disconnect_from(alsa_seq_info->seq, 0, cur->client, cur->port);
				}
				if (cur->client_name != NULL) {
					free(cur->client_name);
				}
				if (cur->port_name != NULL) {
					free(cur->port_name);
				}
				prev = cur;
				cur = cur->next;
				free(prev);
			}
			cur = alsa_seq_info->tx_port;
			while (cur != NULL) {
				if (cur->client_name != NULL) {
					free(cur->client_name);
				}
				if (cur->port_name != NULL) {
					free(cur->port_name);
				}
				prev = cur;
				cur = cur->next;
				free(prev);
			}
			midi_tx_thread_p = 0;
			midi_tx_stopped = 1;
		}

		/* close sequencer only if both rx and tx are not running */
		if ((midi_rx_thread_p == 0) && (midi_tx_thread_p == 0)) {
			if (alsa_seq_info->seq != NULL) {
				snd_seq_disconnect_from(alsa_seq_info->seq,
				                        alsa_seq_info->rx_port->port,
				                        SND_SEQ_CLIENT_SYSTEM,
				                        SND_SEQ_PORT_SYSTEM_ANNOUNCE);
				snd_seq_disconnect_from(alsa_seq_info->seq,
				                        alsa_seq_info->rx_port->port,
				                        SND_SEQ_CLIENT_SYSTEM,
				                        SND_SEQ_PORT_SYSTEM_ANNOUNCE);
				snd_seq_close(alsa_seq_info->seq);
			}
			snd_config_update_free_global();
			if (alsa_seq_info->pfds != NULL) {
				free(alsa_seq_info->pfds);
			}
			free(alsa_seq_info);
			alsa_seq_info = NULL;
		}
	}

	/* Add some guard time, in case MIDI hardware is re-initialized soon. */
	usleep(125000);
}


/*****************************************************************************
 * alsa_seq_rx_thread()
 *
 * ALSA Seq MIDI input thread function.
 * Modifies patch, part, voice, and global parameters.
 *****************************************************************************/
void *
alsa_seq_rx_thread(void *UNUSED(arg))
{
	unsigned char       buffer[SYSEX_BUFFER_SIZE];
	volatile MIDI_EVENT *event;
	struct timespec     now;
	struct sched_param  schedparam;
	pthread_t           thread_id;
	char                thread_name[16];
	snd_seq_event_t     *ev         = NULL;
	unsigned short      cycle_frame = 0;
	unsigned short      rx_index;
	unsigned short      period;
	unsigned char       j;

	/* set realtime scheduling and priority */
	thread_id = pthread_self();
	snprintf(thread_name, 16, "jamrouter%c-rx", ('0' + jamrouter_instance));
	pthread_setname_np(thread_id, thread_name);
	memset(&schedparam, 0, sizeof(struct sched_param));
	schedparam.sched_priority = midi_rx_thread_priority;
	pthread_setschedparam(thread_id, JAMROUTER_SCHED_POLICY, &schedparam);

	/* setup thread cleanup handler */
	pthread_cleanup_push(&alsa_seq_cleanup, (void *)thread_id);

	/* flush MIDI input */
	if (poll(alsa_seq_info->pfds, (nfds_t)(alsa_seq_info->npfds), 0) > 0) {
		while ((snd_seq_event_input(alsa_seq_info->seq, &ev) >= 0) && (ev != NULL));
	}

	/* broadcast the midi ready condition */
	pthread_mutex_lock(&midi_rx_ready_mutex);
	midi_rx_ready = 1;
	pthread_cond_broadcast(&midi_rx_ready_cond);
	pthread_mutex_unlock(&midi_rx_ready_mutex);

	/* MAIN LOOP: poll for midi input and process events */
	while (!midi_rx_stopped && !pending_shutdown) {

		/* set thread cancelation point */
		pthread_testcancel();

		/* poll for new MIDI input */
		//if (snd_seq_event_input_pending(alsa_seq_info->seq, 0) > 0)
		//if (snd_seq_poll_descriptors(alsa_seq_info->seq, alsa_seq_info->pfds,
		//                             (unsigned int)alsa_seq_info->npfds, POLLIN) > 0)
		if (poll(alsa_seq_info->pfds,
		         (nfds_t)(alsa_seq_info->npfds), -1) > 0) {

			/* cycle through all available events */
			while ((snd_seq_event_input(alsa_seq_info->seq, &ev) >= 0) &&
			       ev != NULL) {

				period         = get_midi_period(&now);
				cycle_frame    = get_midi_frame(&period, &now, FRAME_FIX_LOWER | FRAME_LIMIT_UPPER);
				rx_index       = sync_info[period].rx_index;

				event          = get_new_midi_event(A2J_QUEUE);
				event->type    = MIDI_EVENT_NO_EVENT;
				event->channel = ev->data.note.channel;

				switch (ev->type) {

				case SND_SEQ_EVENT_NOTEON:
					event->type           = MIDI_EVENT_NOTE_ON;
					event->channel        = ev->data.note.channel & 0x7F;
					event->note           = ev->data.note.note & 0x7F;
					event->velocity       = ev->data.note.velocity & 0x7F;
					event->bytes          = 3;
					buffer[0]             = (unsigned char)((event->type & 0xF0) |
					                                        (event->channel & 0x0F));
					buffer[1]             = event->note & 0x7F;
					buffer[2]             = event->velocity & 0x7F;
					break;
				case SND_SEQ_EVENT_NOTEOFF:
					/* convert note-off to velocity-0-note-on */
					event->type           = rx_queue_real_note_off ?
						MIDI_EVENT_NOTE_OFF : MIDI_EVENT_NOTE_ON;
					event->channel        = ev->data.note.channel & 0x7F;
					event->note           = ev->data.note.note & 0x7F;
					event->velocity       = ev->data.note.velocity & 0x7F;
					event->bytes          = 3;
					/* convert from alternate note-off velocity to 0 to make software happy */
					if (event->velocity == note_off_velocity) {
						event->velocity = 0x0;
					}
					buffer[0]             = (unsigned char)((event->type & 0xF0) |
					                                        (event->channel & 0x0F));
					buffer[1]             = event->note & 0x7F;
					buffer[2]             = 0x0;
					break;
				case SND_SEQ_EVENT_KEYPRESS:
					event->type           = MIDI_EVENT_AFTERTOUCH;
					event->channel        = ev->data.note.channel & 0x7F;
					event->note           = ev->data.note.note & 0x7F;
					event->velocity       = ev->data.note.velocity & 0x7F;
					event->bytes          = 3;
					buffer[0]             = (unsigned char)((event->type & 0xF0) |
					                                        (event->channel & 0x0F));
					buffer[1]             = event->note & 0x7F;
					buffer[2]             = event->velocity & 0x7F;
					break;
				case SND_SEQ_EVENT_PGMCHANGE:
					event->type           = MIDI_EVENT_PROGRAM_CHANGE;
					event->channel        = ev->data.control.channel & 0x7F;
					event->program        = ev->data.control.value & 0x7F;
					event->bytes          = 2;
					buffer[0]             = (unsigned char)((event->type & 0xF0) |
					                                        (event->channel & 0x0F));
					buffer[1]             = event->program & 0x7F;
					break;
				case SND_SEQ_EVENT_CHANPRESS:
					event->type           = MIDI_EVENT_POLYPRESSURE;
					event->channel        = ev->data.control.channel & 0x7F;
					event->polypressure   = ev->data.control.value & 0x7F;
					event->bytes          = 2;
					buffer[0]             = (unsigned char)((event->type & 0xF0) |
					                                        (event->channel & 0x0F));
					buffer[1]             = event->polypressure & 0x7F;
					break;
				case SND_SEQ_EVENT_CONTROLLER:
					event->type           = MIDI_EVENT_CONTROLLER;
					event->channel        = ev->data.control.channel & 0x7F;
					event->controller     = ev->data.control.param & 0x7F;
					event->value          = ev->data.control.value & 0x7F;
					event->bytes          = 3;
					buffer[0]             = (unsigned char)((event->type & 0xF0) |
					                                        (event->channel & 0x0F));
					buffer[1]             = event->controller & 0x7F;
					buffer[2]             = event->value & 0x7F;
					break;
				case SND_SEQ_EVENT_PITCHBEND:
					event->type           = MIDI_EVENT_PITCHBEND;
					event->channel        = ev->data.control.channel & 0x7F;
					event->lsb            = (ev->data.control.value + 8192) & 0x7F;
					event->msb            = ((ev->data.control.value + 8192) >> 7) & 0x7F;
					event->bytes          = 3;
					buffer[0]             = (unsigned char)((event->type & 0xF0) |
					                                        (event->channel & 0x0F));
					buffer[1]             = event->lsb & 0x7F;
					buffer[2]             = event->msb & 0x7F;
					break;
				case SND_SEQ_EVENT_CONTROL14:
					event->type           = MIDI_EVENT_CONTROL14;
					event->channel        = ev->data.control.channel & 0x7F;
					event->controller     = ev->data.control.param & 0x7F;
					event->lsb            = (ev->data.control.value + 8192) & 0x7F;
					event->msb            = ((ev->data.control.value + 8192) >> 7) & 0x7F;
					event->bytes          = 4;
					buffer[0]             = (unsigned char)((event->type & 0xF0) |
					                                        (event->channel & 0x0F));
					buffer[1]             = event->lsb & 0x7F;
					buffer[2]             = event->msb & 0x7F;
					break;
				case SND_SEQ_EVENT_SYSEX:
					event->type           = MIDI_EVENT_SYSEX;
					event->bytes          = ev->data.ext.len;
					memcpy((void *)(event->data), ev->data.ext.ptr, ev->data.ext.len);
					memcpy(buffer, ev->data.ext.ptr, ev->data.ext.len);
#ifndef WITHOUT_JUNO
					/* translate juno sysex to controllers */
					translate_from_juno(period, A2J_QUEUE,
					                    event, cycle_frame, rx_index);
#endif
					break;
				case SND_SEQ_EVENT_SENSING:
					event->type           = MIDI_EVENT_ACTIVE_SENSING;
					event->bytes          = 1;
					buffer[0]             = (unsigned char)MIDI_EVENT_ACTIVE_SENSING;
					break;
				case SND_SEQ_EVENT_STOP:
					event->type           = MIDI_EVENT_STOP;
					event->bytes          = 1;
					buffer[0]             = (unsigned char)MIDI_EVENT_STOP;
					break;
					/* MIDI system messages */
				case SND_SEQ_EVENT_QFRAME:
				case SND_SEQ_EVENT_CLOCK: /* 0tttvvvv t=type v=value      */
					event->type           = MIDI_EVENT_MTC_QFRAME;
					event->qframe         = (unsigned char)((ev->data.control.param & 0xF0) |
					                                        (ev->data.control.value & 0x0F));
					event->bytes          = 2;
					buffer[0]             = (unsigned char)MIDI_EVENT_MTC_QFRAME;
					buffer[1]             = event->qframe & 0x7F;
					JAMROUTER_DEBUG(DEBUG_CLASS_MIDI_EVENT,
					                "-- Clock msg: %d %d\n",
					                ev->data.control.param,
					                ev->data.control.value);
					break;
				case SND_SEQ_EVENT_SONGPOS:
					event->type           = MIDI_EVENT_SONGPOS;
					event->lsb            = ev->data.control.param & 0x7F;
					event->msb            = ev->data.control.value & 0x7F;
					event->bytes          = 3;
					buffer[0]             = (unsigned char)MIDI_EVENT_SONGPOS;
					buffer[1]             = event->lsb & 0x7F;
					buffer[2]             = event->msb & 0x7F;
					JAMROUTER_DEBUG(DEBUG_CLASS_MIDI_EVENT,
					             "-- Song Position: %d %d\n",
					             ev->data.control.param,
					             ev->data.control.value);
					break;
				case SND_SEQ_EVENT_START:
					event->type           = MIDI_EVENT_START;
					event->bytes          = 1;
					buffer[0]             = (unsigned char)MIDI_EVENT_START;
					JAMROUTER_DEBUG(DEBUG_CLASS_MIDI_EVENT,
					             "-- Start msg: %d %d\n",
					             ev->data.queue.param.d32[0],
					             ev->data.queue.param.d32[1]);
					break;
				case SND_SEQ_EVENT_CONTINUE:
					event->type           = MIDI_EVENT_CONTINUE;
					event->bytes          = 1;
					buffer[0]             = (unsigned char)MIDI_EVENT_CONTINUE;
					JAMROUTER_DEBUG(DEBUG_CLASS_MIDI_EVENT,
					             "-- Continue msg: %d %d\n",
					             ev->data.queue.param.d32[0],
					             ev->data.queue.param.d32[1]);
					break;
				case SND_SEQ_EVENT_SETPOS_TICK:
					event->type           = MIDI_EVENT_SONGPOS;
					event->lsb            = ev->data.control.param & 0x7F;
					event->msb            = ev->data.control.value & 0x7F;
					event->bytes          = 3;
					buffer[0]             = (unsigned char)MIDI_EVENT_SONGPOS;
					buffer[1]             = event->lsb & 0x7F;
					buffer[2]             = event->msb & 0x7F;
					JAMROUTER_DEBUG(DEBUG_CLASS_MIDI_EVENT,
					             "-- SetPos Tick msg: %d %d\n",
					             ev->data.queue.param.d32[0],
					             ev->data.queue.param.d32[1]);
					break;
				case SND_SEQ_EVENT_TICK:
					event->type           = MIDI_EVENT_TICK;
					event->bytes          = 1;
					buffer[0]             = (unsigned char)MIDI_EVENT_TICK;
					JAMROUTER_DEBUG(DEBUG_CLASS_MIDI_EVENT,
					             "-- Tick msg: %d %d\n",
					             ev->data.queue.param.d32[0],
					             ev->data.queue.param.d32[1]);
					break;
				default:
					event->bytes          = 0;
					JAMROUTER_DEBUG(DEBUG_CLASS_MIDI_EVENT,
					             "*** WARNING:  Unhandled ALSA Seq event!  type=%d  ***\n",
					             ev->type);
					break;
				}

				/* queue event for jack thread */
				if (event->type != MIDI_EVENT_NO_EVENT) {
					/* queue notes off for the all-notes-off controller */
					/* or any of the all-sound-off controllers */
					if ( (event->controller >= 0x78) &&
					     (event->type       == MIDI_EVENT_CONTROLLER) ) {
						queue_notes_off(period, A2J_QUEUE, event->channel, cycle_frame, rx_index);
					}
					/* otherwise, queue event as is */
					else {
						queue_midi_event(period, A2J_QUEUE, event, cycle_frame, rx_index, 0);
					}

					if (debug) {
						for (j = 0; j < event->bytes; j++) {
							JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
							                DEBUG_COLOR_LTBLUE "%02X " DEBUG_COLOR_DEFAULT,
							                buffer[j]);
						}
						JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
						                DEBUG_COLOR_LTBLUE "[%d] " DEBUG_COLOR_DEFAULT,
						                cycle_frame);
					}
				}

				snd_seq_free_event(ev);
				ev = NULL;

			} /* while() */

			if (check_active_sensing_timeout(period, A2J_QUEUE) == ACTIVE_SENSING_STATUS_TIMEOUT) {
				period         = get_midi_period(&now);
				cycle_frame    = get_midi_frame(&period, &now, FRAME_LIMIT_LOWER | FRAME_LIMIT_UPPER);
				rx_index       = sync_info[period].rx_index;
				for (j = 0; j < 16; j++) {
					queue_notes_off(period, A2J_QUEUE, j, cycle_frame, rx_index);
				}
			}
		} /* if (poll()) */
	} /* while () */

	/* execute cleanup handler and remove it */
	pthread_cleanup_pop(1);

	/* end of MIDI thread */
	pthread_exit(NULL);
	return NULL;
}


/*****************************************************************************
 * alsa_seq_tx_thread()
 *
 * ALSA Seq MIDI output thread function.
 *****************************************************************************/
void *
alsa_seq_tx_thread(void *UNUSED(arg))
{
	snd_seq_event_t     ev;
	unsigned char       buffer[SYSEX_BUFFER_SIZE];
	char                thread_name[16];
	volatile MIDI_EVENT *event;
	volatile MIDI_EVENT *next               = NULL;
	volatile MIDI_EVENT *cur;
	struct timespec     now;
	struct sched_param  schedparam;
	pthread_t           thread_id;
	unsigned char       first;
	unsigned char       sleep_once;
#ifdef ENABLE_DEBUG
	unsigned short      event_latency;
	unsigned short      end_frame;
	unsigned short      end_period;
	unsigned short      j;
#endif
	unsigned short      cycle_frame         = 0;
	unsigned short      all_notes_off       = 0;
	unsigned short      period;
	unsigned short      last_period         = 0;


	/* set realtime scheduling and priority */
	thread_id = pthread_self();
	snprintf(thread_name, 16, "jamrouter%c-tx", ('0' + jamrouter_instance));
	pthread_setname_np(thread_id, thread_name);
	memset(&schedparam, 0, sizeof(struct sched_param));
	schedparam.sched_priority = midi_tx_thread_priority;
	pthread_setschedparam(thread_id, JAMROUTER_SCHED_POLICY, &schedparam);

	/* setup thread cleanup handler */
	pthread_cleanup_push(&alsa_seq_cleanup, (void *)thread_id);

	//snd_seq_drain_output(alsa_seq_info->seq);

	/* broadcast the midi ready condition */
	pthread_mutex_lock(&midi_tx_ready_mutex);
	midi_tx_ready = 1;
	pthread_cond_broadcast(&midi_tx_ready_cond);
	pthread_mutex_unlock(&midi_tx_ready_mutex);

	period = get_midi_period(&now);
	period = sleep_until_next_period(period, &now);
	cycle_frame = sync_info[period].buffer_period_size;

	/* MAIN LOOP: poll for midi input and process events */
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

		/* Look ahead for optional translation of note off events */
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
					all_notes_off |= (unsigned short)(1 << (cur->channel & 0x0F));
				}
				cur = (MIDI_EVENT *)(cur->next);
			}
		}

		first = 1;
		while ((event != NULL) && (event->state == EVENT_STATE_QUEUED)) {
			if (first) {
				JAMROUTER_DEBUG(DEBUG_CLASS_TX_TIMING,
				                DEBUG_COLOR_YELLOW ": " DEBUG_COLOR_DEFAULT);
			}
			first = 0;

			/* ignore note-off message for any channels with all-notes-off messages. */
			if ( (all_notes_off & (1 << (event->channel & 0x0F))) &&
			     (event->type == MIDI_EVENT_NOTE_ON) && (event->velocity == 0) ) {
				event->bytes = 0;
				JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
				                DEBUG_COLOR_GREEN "-----%X:%02X----- " DEBUG_COLOR_DEFAULT,
				                event->channel, event->note);
			}

			if (event->bytes > 0) {
				/* copy event data into ALSA seq event */
				snd_seq_ev_clear(&ev);
				buffer[event->bytes] = 0x0;
				switch (event->type) {
					/* internal MIDI resync event not needed for JAMRouter's
					   current design, but may be useful in the future. */
					//case MIDI_EVENT_RESYNC:
					//	event->bytes                 = 0;
					//	ev.type                      = SND_SEQ_EVENT_NONE;
					//	JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
					//	                DEBUG_COLOR_GREEN "<<<<<SYNC>>>>> "
					//                  DEBUG_COLOR_DEFAULT);
					//	break;
				case MIDI_EVENT_NOTE_OFF:       // 0x80
					ev.type                   = SND_SEQ_EVENT_NOTEOFF;
					ev.data.note.channel      = event->channel & 0x0F;
					ev.data.note.note         = event->note & 0x7F;
					ev.data.note.velocity     = 0x0;
					buffer[0]                 = (unsigned char)((event->type & 0xF0) |
					                                            (event->channel & 0x0F));
					buffer[1]                 = event->note & 0x7F;
					buffer[2]                 = 0x0;
					break;
				case MIDI_EVENT_NOTE_ON:        // 0x90
					ev.type                   = SND_SEQ_EVENT_NOTEON;
					ev.data.note.channel      = event->channel & 0x0F;
					ev.data.note.note         = event->note & 0x7F;
					ev.data.note.velocity     = event->velocity & 0x7F;
					buffer[0]                 = (unsigned char)((event->type & 0xF0) |
					                                            (event->channel & 0x0F));
					buffer[1]                 = event->note & 0x7F;
					buffer[2]                 = event->velocity & 0x7F;
					break;
				case MIDI_EVENT_AFTERTOUCH:     // 0xA0
					ev.type                   = SND_SEQ_EVENT_KEYPRESS;
					ev.data.note.channel      = event->channel & 0x0F;
					ev.data.note.note         = event->note & 0x7F;
					ev.data.note.velocity     = event->velocity & 0x7F;
					buffer[0]                 = (unsigned char)((event->type & 0xF0) |
					                                            (event->channel & 0x0F));
					buffer[1]                 = event->note & 0x7F;
					buffer[2]                 = event->velocity & 0x7F;
					break;
				case MIDI_EVENT_CONTROLLER:     // 0xB0
					ev.type                   = SND_SEQ_EVENT_KEYPRESS;
					ev.data.control.channel   = event->channel & 0x0F;
					ev.data.control.param     = event->controller & 0x7F;
					ev.data.control.value     = event->value & 0x7F;
					buffer[0]                 = (unsigned char)((event->type & 0xF0) |
					                                            (event->channel & 0x0F));
					buffer[1]                 = event->controller & 0x7F;
					buffer[2]                 = event->value & 0x7F;
					break;
				case MIDI_EVENT_PROGRAM_CHANGE: // 0xC0
					ev.type                   = SND_SEQ_EVENT_PGMCHANGE;
					ev.data.control.channel   = event->channel & 0x0F;
					ev.data.control.value     = event->program & 0x7F;
					buffer[0]                 = (unsigned char)((event->type & 0xF0) |
					                                            (event->channel & 0x0F));
					buffer[1]                 = event->program & 0x7F;
					break;
				case MIDI_EVENT_POLYPRESSURE:   // 0xD0
					ev.type                   = SND_SEQ_EVENT_CHANPRESS;
					ev.data.control.channel   = event->channel & 0x0F;
					ev.data.control.value     = event->polypressure & 0x7F;
					buffer[0]                 = (unsigned char)((event->type & 0xF0) |
					                                            (event->channel & 0x0F));
					buffer[1]                 = event->polypressure & 0x7F;
					break;
				case MIDI_EVENT_PITCHBEND:      // 0xE0
					ev.type                   = SND_SEQ_EVENT_PITCHBEND;
					ev.data.control.channel   = event->channel & 0x0F;
					ev.data.control.value     = (event->lsb & 0x7F) | ((event->msb & 0x7F) << 7);
					buffer[0]                 = (unsigned char)((event->type & 0xF0) |
					                                            (event->channel & 0x0F));
					buffer[1]                 = event->lsb & 0x7F;
					buffer[2]                 = event->msb & 0x7F;
					break;
				//case MIDI_EVENT_CONTROL14:      // not currently implemented.
				//	ev.type                   = SND_SEQ_EVENT_CONTROL14;
				//	ev.data.control.channel   = event->channel & 0x0F;
				//	ev.data.control.value     = (event->lsb & 0x7F) | ((event->msb & 0x7F) << 7);
				//	buffer[0]                 = (unsigned char)((event->type & 0xF0) |
				//	                                            (event->channel & 0x0F));
				//	buffer[1]                 = event->lsb & 0x7F;
				//	buffer[2]                 = event->msb & 0x7F;
				//	break;
				case MIDI_EVENT_SYSEX:          // 0xF0
					ev.type                   = SND_SEQ_EVENT_SYSEX;
					ev.data.ext.len           = event->bytes;
					ev.data.ext.ptr           = (unsigned char *)(event->data);
					memcpy(buffer, (void *)(event->data), event->bytes);
					event->data[event->bytes] = 0x0;
					break;
					/* 3 byte system messages */
				case MIDI_EVENT_SONGPOS:        // 0xF2
					ev.type                   = SND_SEQ_EVENT_SONGPOS;
					ev.data.control.param     = event->lsb & 0x7F;
					ev.data.control.value     = event->msb & 0x7F;
					buffer[0]                 = (unsigned char)MIDI_EVENT_SONGPOS;
					buffer[1]                 = event->lsb & 0x7F;
					buffer[2]                 = event->msb & 0x7F;
					break;
					/* 2 byte system messages */
				case MIDI_EVENT_MTC_QFRAME:     // 0xF1
					ev.type                   = SND_SEQ_EVENT_QFRAME;
					ev.data.control.param     = event->qframe & 0xF0;
					ev.data.control.value     = event->qframe & 0x0F;
					buffer[0]                 = (unsigned char)MIDI_EVENT_MTC_QFRAME;
					buffer[1]                 = event->qframe;
					break;
				case MIDI_EVENT_SONG_SELECT:    // 0xF3
					ev.type                   = SND_SEQ_EVENT_SONGSEL;
					ev.data.control.value     = event->value & 0x7F;
					buffer[0]                 = (unsigned char)MIDI_EVENT_SONG_SELECT;
					buffer[1]                 = event->value & 0x7F;
					break;
					/* 1 byte realtime messages */
				case MIDI_EVENT_BUS_SELECT:     // 0xF5
					ev.type                   = SND_SEQ_EVENT_NONE;
					buffer[0]                 = (unsigned char)MIDI_EVENT_BUS_SELECT;
					break;
				case MIDI_EVENT_TUNE_REQUEST:   // 0xF6
					ev.type                   = SND_SEQ_EVENT_TUNE_REQUEST;
					buffer[0]                 = (unsigned char)MIDI_EVENT_TUNE_REQUEST;
					break;
				case MIDI_EVENT_END_SYSEX:      // 0xF7
					ev.type                   = SND_SEQ_EVENT_NONE;
					buffer[0]                 = (unsigned char)MIDI_EVENT_NO_EVENT;
					break;
				case MIDI_EVENT_TICK:           // 0xF8
					ev.type                   = SND_SEQ_EVENT_TICK;
					ev.data.queue.queue       = SND_SEQ_QUEUE_DIRECT;
					buffer[0]                 = (unsigned char)MIDI_EVENT_TICK;
					break;
				case MIDI_EVENT_START:          // 0xFA
					ev.type                   = SND_SEQ_EVENT_START;
					ev.data.queue.queue       = SND_SEQ_QUEUE_DIRECT;
					buffer[0]                 = (unsigned char)MIDI_EVENT_START;
					break;
				case MIDI_EVENT_CONTINUE:       // 0xFB
					ev.type                   = SND_SEQ_EVENT_CONTINUE;
					ev.data.queue.queue       = SND_SEQ_QUEUE_DIRECT;
					buffer[0]                 = (unsigned char)MIDI_EVENT_CONTINUE;
					break;
				case MIDI_EVENT_STOP:           // 0xFC
					ev.type                   = SND_SEQ_EVENT_STOP;
					ev.data.queue.queue       = SND_SEQ_QUEUE_DIRECT;
					buffer[0]                 = (unsigned char)MIDI_EVENT_STOP;
					break;
				case MIDI_EVENT_ACTIVE_SENSING: // 0xFE
					ev.type                   = SND_SEQ_EVENT_SENSING;
					buffer[0]                 = (unsigned char)MIDI_EVENT_ACTIVE_SENSING;
					break;
				case MIDI_EVENT_SYSTEM_RESET:   // 0xFF
					ev.type                   = SND_SEQ_EVENT_RESET;
					buffer[0]                 = (unsigned char)MIDI_EVENT_SYSTEM_RESET;
					break;
					/* The following are internal message types */
#ifdef MIDI_CLOCK_SYNC
				case MIDI_EVENT_CLOCK:
				case MIDI_EVENT_BPM_CHANGE:
				case MIDI_EVENT_PHASE_SYNC:
#endif /* MIDI_CLOCK_SYNC */
				//case MIDI_EVENT_PARAMETER:    // not currently implemented.
				default:
					ev.type                   = SND_SEQ_EVENT_NONE;
					event->bytes              = 0;
					JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
					                DEBUG_COLOR_GREEN ">%02X< " DEBUG_COLOR_DEFAULT,
					                event->type);
					break;
				}

				/* send event */
				if ((event->bytes > 0) && (ev.type != SND_SEQ_EVENT_NONE)) {
					/*
					  If we are too early for the current event by more then
					  a couple samples, then sleep.
					*/
					if (sleep_once) {
						sleep_until_frame(period, cycle_frame);
						sleep_once = 0;
					}

					/* common to all events */
					snd_seq_ev_set_source(&ev, (unsigned char)(alsa_seq_info->tx_port->port));
					snd_seq_ev_set_subs(&ev);
					ev.queue = SND_SEQ_QUEUE_DIRECT;
					snd_seq_ev_set_direct(&ev);
					snd_seq_event_output_direct(alsa_seq_info->seq, &(ev));

#ifdef ENABLE_DEBUG
					end_period = get_midi_period(&now);
					end_frame = get_midi_frame(&end_period, &now,
					                           FRAME_FIX_LOWER | FRAME_LIMIT_UPPER);

					event_latency = (unsigned short)(end_frame - cycle_frame) &
						sync_info[period].buffer_size_mask;

					if (debug_class & DEBUG_CLASS_STREAM) {
						for (j = 0; j < event->bytes; j++) {
							JAMROUTER_DEBUG(DEBUG_CLASS_STREAM,
							                DEBUG_COLOR_GREEN "%02X " DEBUG_COLOR_DEFAULT,
							                buffer[j]);
						}
					}

					JAMROUTER_DEBUG(DEBUG_CLASS_TIMING,
					                DEBUG_COLOR_GREEN "[%d%+d] " DEBUG_COLOR_DEFAULT,
					                cycle_frame, event_latency);
#endif

					/* optional Tx guard interval between messages */
					if (event_guard_time_usec > 0) {
						jamrouter_usleep(event_guard_time_usec);
					}
				}
			} /* if (event->bytes > 0) */

			/* keep track of next event */
			next = (MIDI_EVENT *)(event->next);

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

	/* end of ALSA seq Tx thread */
	pthread_exit(NULL);
	return NULL;
}
