/*****************************************************************************
 *
 * lash.c
 *
 * JAMRouter:  JACK <--> ALSA MIDI Router
 *
 * Copyright (C) 2010 Anton Kormakov <assault64@gmail.com>
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
#include <libgen.h>
#include <string.h>
#include <lash/lash.h>
#include <jack/jack.h>
#include <asoundlib.h>
#include "jamrouter.h"
#include "driver.h"
#include "debug.h"
#include "lash.h"


lash_client_t   *lash_client;
char            *lash_buffer;
char            *lash_jackname;
char            *lash_project_dir;
char            *lash_project_name;


/*****************************************************************************
 * lash_client_init()
 *****************************************************************************/
int
lash_client_init(int *argc, char ***argv)
{
	lash_event_t    *event;
	char            *lash_name;

	if ((lash_name = malloc(64 * sizeof(char))) == NULL) {
		jamrouter_shutdown("Out of Memory!\n");
	}

	snprintf(lash_name, (64 * sizeof(char)), "jamrouter");
	lash_client = lash_init(lash_extract_args(argc, argv),
	                        lash_name, LASH_Config_File, LASH_PROTOCOL(2, 0));
	if (lash_enabled(lash_client)) {
		event = lash_event_new_with_type(LASH_Client_Name);
		lash_event_set_string(event, lash_name);
		lash_send_event(lash_client, event);

		fprintf(stderr, "LASH client initialized.  (LASH_Client_Name='%s').\n",
		        lash_name);
		return 0;
	}
	fprintf(stderr, "LASH client initialization failed.\n");
	return -1;
}


/*****************************************************************************
 * lash_client_set_jack_name()
 *****************************************************************************/
void
lash_client_set_jack_name(jack_client_t *client)
{
	lash_jackname = jack_get_client_name(client);
	if (lash_enabled(lash_client)) {
		lash_jack_client_name(lash_client, lash_jackname);
	}
}


/*****************************************************************************
 * lash_client_set_alsa_id()
 *****************************************************************************/
void
lash_client_set_alsa_id(snd_seq_t *seq)
{
	if ((midi_driver == MIDI_DRIVER_ALSA_SEQ) && lash_enabled(lash_client)) {
		lash_alsa_client_id(lash_client, (unsigned char)snd_seq_client_id(seq));
	}
}


/*****************************************************************************
 * lash_client_save_args()
 *****************************************************************************/
void
lash_client_save_args(char *lash_dir)
{
	FILE    *args_file;
	char    filename[PATH_MAX];

	snprintf(filename, (PATH_MAX - 1),
	         "%s/jamrouter%d", lash_dir, jamrouter_instance);
	if ((args_file = fopen(filename, "w")) == NULL) {
		JAMROUTER_ERROR("Unable to open %s for write!\n"
		                "command line:  %s\n",
		                filename, jamrouter_cmdline);
		jamrouter_shutdown("Shutting down.\n");
	}
	fprintf(args_file, "%s\n", jamrouter_cmdline);
	fclose(args_file);
}


/*****************************************************************************
 * lash_client_restore_args()
 *****************************************************************************/
void
lash_client_restore_args(char *lash_dir)
{
	FILE    *args_file;
	char    filename[PATH_MAX];
	size_t  len;

	snprintf(filename, (PATH_MAX - 1),
	         "%s/jamrouter%d", lash_dir, jamrouter_instance);
	if ((args_file = fopen(filename, "r")) == NULL) {
		JAMROUTER_ERROR("Unable to open %s for read!\n", filename);
		jamrouter_shutdown("Shutting down.\n");
	}
	if (fgets(jamrouter_cmdline, 512, args_file) == NULL) {
		JAMROUTER_ERROR("Unable to read from %s!\n", filename);
		jamrouter_shutdown("Shutting down.\n");
	}
	fclose(args_file);
	len = strlen(jamrouter_cmdline);
	if (jamrouter_cmdline[len - 1] == '\n') {
		jamrouter_cmdline[len - 1] = '\0';
	}
}


/*****************************************************************************
 * lash_poll_event()
 *****************************************************************************/
int
lash_poll_event(void)
{
	lash_event_t    *event;

	if (lash_disabled || !lash_enabled(lash_client)) {
		return -1;
	}

	while ((event = lash_get_event(lash_client)) != NULL) {

		switch (lash_event_get_type(event)) {

		case LASH_Save_File:
			lash_buffer = (char *) lash_event_get_string(event);
			JAMROUTER_DEBUG(DEBUG_CLASS_SESSION,
			                "lash_poll_event():  LASH_Save_File  dir='%s'\n",
			                lash_buffer);
			lash_client_save_args(lash_buffer);
			lash_send_event(lash_client,
			                lash_event_new_with_type(LASH_Save_File));
			break;

		case LASH_Restore_File:
			lash_buffer = (char *) lash_event_get_string(event);
			JAMROUTER_DEBUG(DEBUG_CLASS_SESSION,
			                "lash_poll_event():  LASH_Restore_File  dir='%s'\n",
			                lash_buffer);
			lash_client_restore_args(lash_buffer);
			lash_send_event(lash_client,
			                lash_event_new_with_type(LASH_Restore_File));
			break;

			/* TODO: support the complete LASH spec. */
		case LASH_Client_Name:
			JAMROUTER_DEBUG(DEBUG_CLASS_SESSION,
			             "LASH received unhandled event 'LASH_Client_Name'\n");
			break;
		case LASH_Jack_Client_Name:
			JAMROUTER_DEBUG(DEBUG_CLASS_SESSION,
			             "LASH received unhandled event 'LASH_Jack_Client_Name'\n");
			break;
		case LASH_Alsa_Client_ID:
			JAMROUTER_DEBUG(DEBUG_CLASS_SESSION,
			             "LASH received unhandled event 'LASH_Alsa_Client_ID'\n");
			break;
		case LASH_Save_Data_Set:
			JAMROUTER_DEBUG(DEBUG_CLASS_SESSION,
			             "LASH received unhandled event 'LASH_Save_Data_Set'\n");
			break;
		case LASH_Restore_Data_Set:
			JAMROUTER_DEBUG(DEBUG_CLASS_SESSION,
			             "LASH received unhandled event 'LASH_Restore_Data_Set'\n");
			break;
		case LASH_Save:
			JAMROUTER_DEBUG(DEBUG_CLASS_SESSION,
			             "LASH received unhandled event 'LASH_Save'\n");
			break;
		case LASH_Server_Lost:
			JAMROUTER_DEBUG(DEBUG_CLASS_SESSION,
			             "LASH received unhandled event 'LASH_Server_Lost'\n");
			break;
		case LASH_Project_Add:
			JAMROUTER_DEBUG(DEBUG_CLASS_SESSION,
			             "LASH received unhandled event 'LASH_Project_Add'\n");
			break;
		case LASH_Project_Remove:
			JAMROUTER_DEBUG(DEBUG_CLASS_SESSION,
			             "LASH received unhandled event 'LASH_Project_Remove'\n");
			break;
		case LASH_Project_Dir:
			JAMROUTER_DEBUG(DEBUG_CLASS_SESSION,
			             "LASH received unhandled event 'LASH_Project_Dir'\n");
			break;
		case LASH_Project_Name:
			JAMROUTER_DEBUG(DEBUG_CLASS_SESSION,
			             "LASH received unhandled event 'LASH_Project_Name'\n");
			break;
		case LASH_Client_Add:
			JAMROUTER_DEBUG(DEBUG_CLASS_SESSION,
			             "LASH received unhandled event 'LASH_Client_Add'\n");
			break;
		case LASH_Client_Remove:
			JAMROUTER_DEBUG(DEBUG_CLASS_SESSION,
			             "LASH received unhandled event 'LASH_Client_Remove'\n");
			break;
		case LASH_Percentage:
			JAMROUTER_DEBUG(DEBUG_CLASS_SESSION,
			             "LASH received unhandled event 'LASH_Percentage'\n");
			break;
		case LASH_Quit:
			JAMROUTER_DEBUG(DEBUG_CLASS_SESSION,
			             "LASH LASH_Quit!  Shutting down.\n");
			pending_shutdown = 1;
			break;
			//              case LASH_Event_Unknown:
		default:
			JAMROUTER_DEBUG(DEBUG_CLASS_SESSION,
			             "LASH received unhandled event 'LASH_Event_Unknown'\n");
			break;
		}

		lash_event_destroy(event);
	}
	return 0;
}
