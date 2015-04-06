/*****************************************************************************
 *
 * jamrouter.c
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
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <asoundlib.h>
#include "jamrouter.h"
#include "driver.h"
#include "alsa_seq.h"
#include "jack.h"
#include "midi_event.h"
#include "timekeeping.h"
#include "debug.h"


#ifndef WITHOUT_LASH
# include "lash.h"
#endif


/* command line options */
#define HAS_ARG     1
#ifdef WITHOUT_JUNO
# define NUM_OPTS    (39 + 1)
#else
# define NUM_OPTS    (41 + 1)
#endif
static struct option long_opts[] = {
#ifndef WITHOUT_JUNO
	{ "juno",            0,       NULL, 'J' },
	{ "echosysex",       0,       NULL, 's' },
#endif
	{ "midi-driver",     HAS_ARG, NULL, 'M' },
	{ "device",          HAS_ARG, NULL, 'D' },
	{ "rx-device",       HAS_ARG, NULL, 'r' },
	{ "tx-device",       HAS_ARG, NULL, 't' },
	{ "rx-latency",      HAS_ARG, NULL, 'x' },
	{ "tx-latency",      HAS_ARG, NULL, 'X' },
	{ "byte-guard-time", HAS_ARG, NULL, 'g' },
	{ "event-guard-time",HAS_ARG, NULL, 'G' },
	{ "input-port",      HAS_ARG, NULL, 'i' },
	{ "output-port",     HAS_ARG, NULL, 'o' },
	{ "jitter-correct",  0,       NULL, 'j' },
	{ "keymap",          HAS_ARG, NULL, 'k' },
	{ "pitchmap",        HAS_ARG, NULL, 'p' },
	{ "pitchcontrol",    HAS_ARG, NULL, 'q' },
	{ "echotrans",       0,       NULL, 'e' },
	{ "sysexterminator", HAS_ARG, NULL, 'T' },
	{ "extraterminator", HAS_ARG, NULL, 'U' },
	{ "activesensing",   HAS_ARG, NULL, 'A' },
	{ "runningstatus",   0,       NULL, 'R' },
	{ "rxrealnoteoff",   0,       NULL, '0' },
	{ "txrealnoteoff",   0,       NULL, 'F' },
	{ "txallnotesoff",   0,       NULL, 'f' },
	{ "noteonvelocity",  HAS_ARG, NULL, 'n' },
	{ "noteoffvelocity", HAS_ARG, NULL, 'N' },
	{ "rx-priority",     HAS_ARG, NULL, 'y' },
	{ "tx-priority",     HAS_ARG, NULL, 'Y' },
	{ "debug",           HAS_ARG, NULL, 'd' },
	{ "uuid",            HAS_ARG, NULL, 'u' },
	{ "list",            0,       NULL, 'l' },
	{ "help",            0,       NULL, 'h' },
	{ "version",         0,       NULL, 'v' },
	{ "disable-lash",    0,       NULL, 'L' },
	{ "lash-project",    HAS_ARG, NULL, 'P' },
	{ "lash-server",     HAS_ARG, NULL, 'S' },
	{ "lash-id",         HAS_ARG, NULL, 'I' },
	{ "jackdll1",        0,       NULL, '1' },
	{ "jackdll2",        0,       NULL, '2' },
	{ "jackdll3",        0,       NULL, '3' },
	{ "phase-lock",      HAS_ARG, NULL, 'z' },
	{ 0,                 0,       NULL, 0 }
};


char            jamrouter_cmdline[512]        = "\0";
char            jamrouter_full_cmdline[512]   = "\0";

pthread_t       debug_thread_p                = 0;
pthread_t       midi_rx_thread_p              = 0;
pthread_t       midi_tx_thread_p              = 0;
pthread_t       jack_thread_p                 = 0;

int             midi_rx_thread_priority       = MIDI_RX_THREAD_PRIORITY;
int             midi_tx_thread_priority       = MIDI_TX_THREAD_PRIORITY;

char            *midi_rx_port_name            = NULL;
char            *midi_tx_port_name            = NULL;
char            *jack_input_port_name         = NULL;
char            *jack_output_port_name        = NULL;

int             lash_disabled                 = 0;
int             sample_rate                   = 0;
int             jamrouter_instance            = 0;
int             pending_shutdown              = 0;
int             tx_prefer_all_notes_off       = 0;
int             tx_prefer_real_note_off       = 0;
int             rx_queue_real_note_off        = 0;
int             echotrans                     = 0;
int             active_sensing_mode           = ACTIVE_SENSING_MODE_ON;
int             use_running_status            = 0;
int             byte_guard_time_usec          = 0;
int             event_guard_time_usec         = 0;
int             rx_latency_periods            = 0;
int             tx_latency_periods            = 0;
int             jitter_correct_mode           = 0;
#ifndef WITHOUT_JACK_DLL
int             jack_dll_level                = 0;
#endif
#ifndef WITHOUT_JUNO
int             translate_juno_sysex          = 0;
int             echosysex                     = 0;
#endif

unsigned char   sysex_terminator              = 0xF7;
unsigned char   sysex_extra_terminator        = 0xF7;
unsigned char   note_on_velocity              = 0x00;
unsigned char   note_off_velocity             = 0x00;

unsigned char   keymap_tx_channel[16]         =
	{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
unsigned char   keymap_tx_controller[16]      =
	{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

unsigned char   pitchmap_tx_channel[16]       =
	{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
unsigned char   pitchmap_center_note[16]      =
	{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
unsigned char   pitchmap_bend_range[16]       =
	{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

unsigned char   pitchcontrol_tx_channel[16]   =
	{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
unsigned char   pitchcontrol_controller[16]   =
	{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };


/*****************************************************************************
 * showusage()
 *****************************************************************************/
void
showusage(char *argvzero)
{
	printf("Usage:  %s [options] \n\n", argvzero);
	printf("JAMRouter Options:\n\n"
	       " -v, --version           Display version and exit.\n"
	       " -h, --help              Display this help message.\n"
	       " -u, --uuid=             Set UUID for JACK Session handling.\n"
	       " -d, --debug=            Can be repeated.  Choose from: full, init, driver,\n"
	       "                           stream, timing, tx-timing, note, event, session.\n"
#ifndef WITHOUT_LASH
	       "LASH Options:\n\n"
	       " -P, --lash-project=     LASH project name.\n"
	       " -S, --lash-server=      LASH server address.\n"
	       " -I, --lash-id=          LASH client ID.\n"
	       " -L, --disable-lash      Disable LASH completely for the current session.\n"
#endif
	       "\nMIDI Device / Port Options:\n\n"
	       " -l, --list              Scan and list MIDI devices.\n"
	       " -M, --midi-driver=      MIDI driver:  seq, raw, generic, or dummy.\n"
	       " -D, --device=           MIDI Rx/Tx port or device name (driver specific).\n"
	       " -r, --rx-device=        MIDI Rx port or device name (driver specific).\n"
	       " -t, --tx-device=        MIDI Tx port or device name (driver specific).\n"
	       " -x, --rx-latency=       MIDI Rx latency periods (default 1 for buf > 128).\n"
	       " -X, --tx-latency=       MIDI Tx latency periods (default 1 for buf > 128).\n"
	       " -g, --byte-guard-time=  Guard time in microseconds after Tx of MIDI byte.\n"
	       " -G, --event-guard-time= Guard time in microseconds after Tx of MIDI event.\n"
	       " -i, --input-port=       JACK MIDI Input port name.\n"
	       " -o, --output-port=      JACK MIDI Output port name.\n"
	       " -y, --rx-priority=      Realtime thread priority for MIDI Rx thread.\n"
	       " -Y, --tx-priority=      Realtime thread priority for MIDI Tx thread.\n\n"
	       "MIDI Message Translation Options:\n\n"
	       " -A, --activesensing=    Active-Sensing mode:  on, thru, drop  (default on).\n"
	       " -R, --runningstatus     When possible, omit Running-Status byte on MIDI Tx.\n"
	       " -T, --sysexterminator=  <hex-val>  End SysEx byte if not the standard 0xF7.\n"
	       " -U, --extraterminator=  <hex-val>  2nd byte for 2-byte SysEx terminators.\n"
	       " -n, --noteonvelocity=   <hex-val>  Note-On Velocity for MIDI Tx.\n"
	       " -N, --noteoffvelocity=  <hex-val>  Note-Off Velocity for MIDI Tx.\n"
           " -0, --rxrealnoteoff     Send Note-Off for Velocity-0-Note-On on JACK output.\n"
	       " -F, --txrealnoteoff     Send Note-Off for Velocity-0-Note-On on MIDI Tx.\n\n"
	       " -f, --txallnotesoff     With no notes left in play, translate multiple Note-\n"
	       "                           Off messages to All-Notes-Off Controller for Tx.\n\n"
	       " -k, --keymap=           <rx-chan>,<tx-chan>,<controller>\n"
	       "                           Map MIDI notes to controller on alternate channel.\n"
	       "                           (Can be repeated once per Rx channel.)\n\n"
	       " -p, --pitchmap=         <rx-chan>,<tx-chan>,<center-note>,<pitchbend-range>\n"
	       "                           Map MIDI notes to pitchbend on alternate channel.\n"
	       "                           (Can be repeated once per Rx channel.)\n\n"
	       " -q, --pitchcontrol=     <rx-chan>,<tx-chan>,<controller>\n"
	       "                           Map pitchbend to controller on alternate channel.\n"
	       "                           (Can be repeated once per Rx channel.)\n\n"
	       " -e, --echotrans         Echo translated pitchbend and controller messages\n"
	       "                           to JACK MIDI output port for sequencer recording.\n\n"
#ifndef WITHOUT_JUNO
	       "JUNO-106 Translation Options:\n\n"
	       " -J, --juno              Enable Juno-106 SysEx <--> Controller translation.\n"
	       "                           (See " PACKAGE_DATA_DIR "/doc/juno-106.txt).\n\n"
	       " -s, --echosysex         Echo translated Juno-106 SysEx messages\n"
	       "                           to JACK MIDI output port for sequencer recording.\n\n"
#endif
	       "Experimental Options:\n\n"
	       " -j, --jitter-correct    Rx jitter correction mode.\n"
	       " -z, --phase-lock=       JACK wakeup phase in MIDI Rx/Tx period (.06-.94).\n\n"
#ifndef WITHOUT_JACK_DLL
	       " -1, --jackdll1          JACK DLL timing level 1:  Sync PLL to DLL only.\n"
	       " -2, --jackdll2          JACK DLL timing level 2:  JACK DLL Sync and Rx.\n"
	       " -3, --jackdll3          JACK DLL timing level 3:  JACK DLL Sync and Rx/Tx.\n"
#endif
	       "\nJAMRouter:  JACK <--> ALSA MIDI Router  ver. " PACKAGE_VERSION "\n"
	       "  (C) 2015 William Weston <william.h.weston@gmail.com>,\n"
	       "Distributed under the terms of the GNU GENERAL Public License, Version 3.\n"
	       "  (See AUTHORS, LICENSE, and GPL-3.0.txt for details.)\n");
}


/*****************************************************************************
 * get_instance_num()
 *****************************************************************************/
int
get_instance_num(void)
{
	char                buf[1024];
	char                filename[PATH_MAX];
	FILE                *cmdfile;
	DIR                 *slashproc;
	struct dirent       *procdir;
	int                 j;
	int                 i;
	int                 instances[20];
	char                *p;
	char                *q;

	for (j = 1; j <= 16; j++) {
		instances[j] = 0;
	}

	if ((slashproc = opendir ("/proc")) == NULL) {
		fprintf (stderr, "Unable to read /proc filesystem!\n");
		exit (1);
	}
	while ((procdir = readdir (slashproc)) != NULL) {
		if (procdir->d_type != DT_DIR) {
			continue;
		}
		snprintf (filename, PATH_MAX, "/proc/%s/cmdline", procdir->d_name);
		p = q = (char *)(procdir->d_name);
		while (isdigit (*q) && ((q - p) < 8)) {
			q++;
		}
		if (*q != '\0') {
			continue;
		}
		if ((cmdfile = fopen (filename, "rt")) == NULL) {
			continue;
		}
		if (fread (buf, sizeof (char), sizeof (buf), cmdfile) <= 0) {
			fclose (cmdfile);
			continue;
		}
		fclose (cmdfile);
		if (strncmp (buf, "jamrouter", 9) != 0) {
			continue;
		}
		if ((buf[10] >= '0') && (buf[10] <= '9')) {
			i = (10 * (buf[9] - '0')) + (buf[10] - '0');
		}
		else {
			i = (buf[9] - '0');
		}
		if ((i < 1) || (i > 16)) {
			continue;
		}
		instances[i] = 1;
	}
	closedir (slashproc);
	for (j = 1; j <= 16; j++) {
		if (instances[j] == 0) {
			return j;
		}
	}

	return -1;
}


/*****************************************************************************
 * get_color_terminal()
 *****************************************************************************/
char *
get_color_terminal(void)
{
	char *term        = secure_getenv("TERM");
	char *colorterm   = secure_getenv("COLORTERM");

	if ((colorterm != NULL) && (colorterm[0] != '\0')) {
		return colorterm;
	}
	if ((term != NULL) && (term[0] != '\0')) {
		return term;
	}

	return NULL;
}


/*****************************************************************************
 * jamrouter_shutdown()
 *
 * Main shutdown function.  Can be called from anywhere to cleanly shutdown.
 *****************************************************************************/
void
jamrouter_shutdown(const char *msg)
{
	/* output message from caller */
	if (msg != NULL) {
		fprintf(stderr, "%s", msg);
	}

	/* set the global shutdown flag */
	pending_shutdown = 1;
}


/*****************************************************************************
 * jamrouter_signal_handler()
 *****************************************************************************/
static void
jamrouter_signal_handler(int i)
{
	fprintf(stderr, "JAMRouter received signal %s.  Shutting down.\n",
	        strsignal(i));
	pending_shutdown = 1;
	sleep(1);
	if (midi_rx_thread_p != 0) {
		pthread_cancel(midi_rx_thread_p);
	}
	if (midi_tx_thread_p != 0) {
		pthread_cancel(midi_tx_thread_p);
	}
}


/*****************************************************************************
 * init_signal_handlers()
 *****************************************************************************/
int
init_signal_handlers(void)
{
	int                 signals[14] = {
		SIGHUP,  SIGINT,  SIGQUIT, SIGILL,  SIGABRT, SIGFPE,  SIGSEGV,
		SIGPIPE, SIGALRM, SIGTERM, SIGUSR1, SIGUSR2, SIGCHLD, 0
	};
	struct sigaction    action;
	int                 j;

	action.sa_handler = jamrouter_signal_handler;

	for (j = signals[0]; signals[j] != 0; j++) {
		if (sigaction(signals[j], &action, NULL) < 0) {
			return 0;
		}
	}
	return 1;
}


/*****************************************************************************
 * init_rt_mutex()
 *****************************************************************************/
void
init_rt_mutex(pthread_mutex_t *mutex, int rt)
{
	pthread_mutexattr_t     attr;

	/* set attrs for realtime mutexes */
	pthread_mutexattr_init(&attr);
#ifdef HAVE_PTHREAD_MUTEXATTR_SETPROTOCOL
	if (rt) {
		pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
	}
#endif

	/* init mutex with attrs */
	pthread_mutex_init(mutex, &attr);
}


/*****************************************************************************
 * hex_to_byte()
 *****************************************************************************/
unsigned char
hex_to_byte(char *hex)
{
	char            *p   = hex;
	unsigned char   byte = 0x0;
	short           j;

	if ( (*(p) == '0') &&
	     ( ((*(p + 1)) == 'x') || ((*(p + 1)) == 'X') ) ) {
		p++;
		p++;
	}
	for (j = 0; j < 2; j++) {
		if (((*(p) >= '0')) || (*(p) <= '9')) {
			byte |= (unsigned char)(((*(p) - '0') << (j << 2)) & 0xFF);
		}
		else if (((*(p) >= 'a')) || (*(p) <= 'f')) {
			byte |= (unsigned char)(((*(p) + 0x0A - 'a') << (j << 2)) & 0xFF);
		}
		else if (((*(p) >= 'A')) || (*(p) <= 'F')) {
			byte |= (unsigned char)(((*(p) + 0x0A - 'A') << (j << 2)) & 0xFF);
		}
	}
	return byte;
}


/*****************************************************************************
 * main()
 *
 * Parse command line, load patch, start midi_tx, midi_rx, and jack threads.
 *****************************************************************************/
int
main(int argc, char **argv)
{
	char            thread_name[16];
	char            opts[NUM_OPTS * 2 + 1];
	struct option   *op;
	char            *cp;
	char            *p;
	char            *term;
	char            *tokbuf;
	int             c;
	int             j                       = 0;
	int             ret                     = 0;
	int             saved_errno;
	int             argcount                = 0;
	char            **argvals               = argv;
	char            **envp                  = environ;
	char            *argvend                = (char *)argv;
	size_t          argsize;
	unsigned char   rx_channel;

	setlocale(LC_ALL, "C");

	jamrouter_instance = get_instance_num();
	fprintf(stderr, "Starting jamrouter instance %d.\n", jamrouter_instance);

	/* Start debug thread.  debug_class is not set until arguemnts are parsed,
	   so use fprintf() until then. */
	if ((ret = pthread_create(&debug_thread_p, NULL, &jamrouter_debug_thread, NULL)) != 0) {
		fprintf(stderr, "***** ERROR:  Unable to start debug thread.\n");
	}

	/* lock down memory (rt hates page faults) */
	if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
		saved_errno = errno;
		fprintf(stderr, "Unable to unlock memory:  errno=%d (%s)\n",
		        saved_errno, strerror(saved_errno));
	}

	/* init lash client */
#ifndef WITHOUT_LASH
	for (j = 0; j < argc; j++) {
		if ((strcmp(argv[j], "-L") == 0) || (strcmp(argv[j], "--disable-lash") == 0) ||
		    (strcmp(argv[j], "-h") == 0) || (strcmp(argv[j], "--help") == 0) ||
		    (strcmp(argv[j], "-l") == 0) || (strcmp(argv[j], "--list") == 0) ||
		    (strcmp(argv[j], "-v") == 0) || (strcmp(argv[j], "--version") == 0) ||
		    (strcmp(argv[j], "-D") == 0) || (strcmp(argv[j], "--session-dir") == 0) ||
		    (strcmp(argv[j], "-u") == 0) || (strcmp(argv[j], "--uuid") == 0)) {
			lash_disabled = 1;
			break;
		}
	}
	if (!lash_disabled) {
		snprintf(thread_name, 16, "jamrouter%c-lash", ('0' + jamrouter_instance));
		pthread_setname_np(pthread_self(), thread_name);
		if (lash_client_init(&argc, &argv) == 0) {
			lash_poll_event();
		}
		snprintf(thread_name, 16, "jamrouter%c-main", ('0' + jamrouter_instance));
		pthread_setname_np(pthread_self(), thread_name);
	}
#endif

	/* startup initializations */
	init_midi_event_queue();
	init_jack_audio_driver();
	select_midi_driver(NULL, DEFAULT_MIDI_DRIVER);

	/* save original command line for session handling */
	if (jamrouter_cmdline[0] == '\0') {
		jamrouter_cmdline[0] = '\0';
		for (j = 0; j < argc; j++) {
			strcat(&(jamrouter_cmdline[0]), argv[j]);
			strcat(&(jamrouter_cmdline[0]), " ");
		}
		jamrouter_cmdline[strlen(jamrouter_cmdline) - 1] = '\0';
		term = get_color_terminal();
		if (term == NULL) {
			strcpy(jamrouter_full_cmdline, jamrouter_cmdline);
		}
		else {
			snprintf(jamrouter_full_cmdline, 512, "%s -e \"%s \"",
			         term, jamrouter_cmdline);
		}
		argcount = argc;
		argvals  = argv;
	}
	/* command line args supplied by session manager */
	else {
		argcount = 0;
		cp = strdup(jamrouter_cmdline);
		p = cp;
		while ((p = index(p, ' ')) != NULL) {
			p++;
			argcount++;
		}
		if ((argvals = malloc(((size_t)(argcount) + 1UL) *
		                      (size_t)sizeof(char *))) == NULL) {
			fprintf(stderr, "Out of Memory!\n");
			return -1;
		}
		if ((tokbuf = alloca(strlen(jamrouter_cmdline) * 4)) == NULL) {
			fprintf(stderr, "Out of Memory!\n");
			return -1;
		}
		while ((p = strtok_r(cp, " ", &tokbuf)) != NULL) {
			cp = NULL;
			argvals[j++] = p;
		}
		argvals[argcount] = NULL;
	}

	/* build the short option string */
	cp = opts;
	for (op = long_opts; op < &long_opts[NUM_OPTS]; op++) {
		*cp++ = (char) op->val;
		if (op->has_arg) {
			*cp++ = ':';
		}
	}

	/* handle options */
	for (;;) {
		c = getopt_long(argcount, argvals, opts, long_opts, NULL);
		if (c == -1) {
			break;
		}

		switch (c) {
		case 'M':   /* MIDI driver */
			select_midi_driver(optarg, -1);
			break;
		case 'D':   /* MIDI Rx/Tx port/device */
			midi_rx_port_name = strdup(optarg);
			midi_tx_port_name = strdup(optarg);
			break;
		case 'r':   /* MIDI Rx port/device */
			midi_rx_port_name = strdup(optarg);
			break;
		case 't':   /* MIDI Tx port/device */
			midi_tx_port_name = strdup(optarg);
			break;
		case 'x':   /* MIDI Rx latency periods */
			rx_latency_periods = atoi(optarg);
			break;
		case 'X':   /* MIDI Tx latency periods */
			tx_latency_periods = atoi(optarg);
			break;
		case 'g':   /* Tx byte guard time in usec */
			byte_guard_time_usec = atoi(optarg);
			break;
		case 'G':   /* Tx event guard time in usec */
			event_guard_time_usec = atoi(optarg);
			break;
		case 'i':   /* JACK MIDI input port */
			jack_input_port_name = strdup(optarg);
			break;
		case 'o':   /* JACK MIDI output port */
			jack_output_port_name = strdup(optarg);
			break;
		case 'j':   /* Jitter correction mode */
			jitter_correct_mode = 1;
			break;
		case 'z':   /* JACK wake phase within MIDI Rx/Tx period */
			setting_midi_phase_lock = (timecalc_t)(atof(optarg));
			if (setting_midi_phase_lock < (timecalc_t)(0.0625)) {
				setting_midi_phase_lock = (timecalc_t)(0.0625);
			}
			else if (setting_midi_phase_lock > (timecalc_t)(0.9375)) {
				setting_midi_phase_lock = (timecalc_t)(0.9375);
			}
			break;
#ifndef WITHOUT_JACK_DLL
		case '3':   /* JACK DLL timing level 3 */
			jack_dll_level++;
			/* fall-through */
		case '2':   /* JACK DLL timing level 2 */
			jack_dll_level++;
			/* fall-through */
		case '1':   /* JACK DLL timing level 1 */
			jack_dll_level++;
			break;
#endif
		case 'k':   /* key to controller mapping */
			if (optarg != NULL) {
				if ((tokbuf = alloca(strlen((const char *)optarg) * 4)) == NULL) {
					jamrouter_shutdown("Out of memory!\n");
				}
				if ((p = strtok_r(optarg, ",", &tokbuf)) != NULL) {
					rx_channel = (atoi(p) - 1) & 0x0F;
					if ((p = strtok_r(NULL, ",", &tokbuf)) != NULL) {
						keymap_tx_channel[rx_channel] = (atoi(p) - 1) & 0x0F;
						if ((p = strtok_r(NULL, ",", &tokbuf)) != NULL) {
							keymap_tx_controller[rx_channel] = atoi(p) & 0x7F;
						}
					}
					JAMROUTER_DEBUG(DEBUG_CLASS_INIT, "Key --> Controller Map:  "
					                "rx_channel=%0d  tx_channel=%d  tx_cc=%d\n",
					                rx_channel + 1,
					                keymap_tx_channel[rx_channel] + 1,
					                keymap_tx_controller[rx_channel]);
				}
			}
			break;
		case 'p':   /* key to pitchbend translation */
			if (optarg != NULL) {
				if ((tokbuf = alloca(strlen((const char *)optarg) * 4)) == NULL) {
					jamrouter_shutdown("Out of memory!\n");
				}
				if ((p = strtok_r(optarg, ",", &tokbuf)) != NULL) {
					rx_channel = (atoi(p) - 1) & 0x0F;
					if ((p = strtok_r(NULL, ",", &tokbuf)) != NULL) {
						pitchmap_tx_channel[rx_channel] = (atoi(p) - 1) & 0x0F;
						if ((p = strtok_r(NULL, ",", &tokbuf)) != NULL) {
							pitchmap_center_note[rx_channel] = atoi(p) & 0x7F;
							if ((p = strtok_r(NULL, ",", &tokbuf)) != NULL) {
								pitchmap_bend_range[rx_channel] = atoi(p) & 0x7F;
							}
						}
					}
					JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
					                "Key --> Pitchbend Map:  "
					                "rx_chan=%0d  tx_chan=%d  center=%d  range=%d\n",
					                rx_channel + 1, pitchmap_tx_channel[rx_channel] + 1,
					                pitchmap_center_note[rx_channel],
					                pitchmap_bend_range[rx_channel]);
				}
			}
			break;
		case 'q':   /* pitchbend to controller translation */
			if (optarg != NULL) {
				if ((tokbuf = alloca(strlen((const char *)optarg) * 4)) == NULL) {
					jamrouter_shutdown("Out of memory!\n");
				}
				if ((p = strtok_r(optarg, ",", &tokbuf)) != NULL) {
					rx_channel = (atoi(p) - 1) & 0x0F;
					if ((p = strtok_r(NULL, ",", &tokbuf)) != NULL) {
						pitchcontrol_tx_channel[rx_channel] = (atoi(p) - 1) & 0x0F;
						if ((p = strtok_r(NULL, ",", &tokbuf)) != NULL) {
							pitchcontrol_controller[rx_channel] = atoi(p) & 0x7F;
						}
					}
					JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
					                "Pitchbend --> Controller Map:  "
					                "rx_chan=%0d  tx_chan=%d  controller=%d\n",
					                rx_channel + 1, pitchcontrol_tx_channel[rx_channel] + 1,
					                pitchcontrol_controller[rx_channel]);
				}
			}
			break;
#ifndef WITHOUT_JUNO
		case 'J':   /* Juno-106 sysex controller translation */
			translate_juno_sysex = 1;
			break;
		case 's':   /* echo sysex translations back to originator */
			echosysex = 1;
			break;
#endif
		case 'e':   /* echo pitchbend and controller translations back to originator */
			echotrans = 1;
			break;
		case 'T':   /* alternate sysex terminator byte */
			sysex_terminator = hex_to_byte(optarg);
			break;
		case 'U':   /* alternate sysex terminator byte */
			sysex_extra_terminator = hex_to_byte(optarg);
			break;
		case 'A':   /* Active-Sensing mode */
			if (strcmp(optarg, "on") == 0) {
				active_sensing_mode = ACTIVE_SENSING_MODE_ON;
			}
			else if (strcmp(optarg, "thru") == 0) {
				active_sensing_mode = ACTIVE_SENSING_MODE_THRU;
			}
			else if (strcmp(optarg, "drop") == 0) {
				active_sensing_mode = ACTIVE_SENSING_MODE_DROP;
			}
			break;
		case 'R':   /* Omit running status byte on MIDI Tx */
			use_running_status = 1;
			break;
		case 'n':   /* Note-On Velocity */
			note_on_velocity = hex_to_byte(optarg);
			break;
		case 'N':   /* Note-Off Velocity */
			note_off_velocity = hex_to_byte(optarg);
			break;
		case 'f':   /* Send multiple Note-Off messages as All-Notes-Off */
			tx_prefer_all_notes_off = 1;
			break;
		case 'F':   /* Tx send real Note-Off instead of Velocity-0-Note-On */
			tx_prefer_real_note_off = 1;
			break;
		case '0':   /* Rx queue real Note-Off instead of Velocity-0-Note-On */
			rx_queue_real_note_off = 1;
			break;
		case 'y':   /* MIDI Rx thread priority */
			if ((midi_rx_thread_priority = atoi(optarg)) <= 0) {
				midi_rx_thread_priority = MIDI_RX_THREAD_PRIORITY;
			}
			break;
		case 'Y':   /* MIDI Tx thread priority */
			if ((midi_tx_thread_priority = atoi(optarg)) <= 0) {
				midi_tx_thread_priority = MIDI_TX_THREAD_PRIORITY;
			}
			break;
		case 'd':   /* debug */
			debug = 1;
			for (j = 0; debug_class_list[j].name != NULL; j++) {
				if (strcmp(debug_class_list[j].name, optarg) == 0) {
					debug_class |= debug_class_list[j].id;
				}
			}
			break;
		case 'v':   /* version */
			printf("jamrouter-%s\n", PACKAGE_VERSION);
			return 0;
		case 'L':   /* disable lash */
			lash_disabled = 1;
			break;
		case 'l':   /* list midi devices */
			scan_midi();
			return 0;
		case 'u':   /* jack session uuid */
			jack_session_uuid = strdup(optarg);
			break;
		case '?':
		case 'h':   /* help */
		default:
			showusage(argv[0]);
			return -1;
		}
	}

	/* Rewrite process title */
	argcount = argc;
	argvals  = argv;
	for (j = 0; j <= argcount; j++) {
		if ((j == 0) || ((argvend + 1) == argvals[j])) {
			argvend = argvals[j] + strlen(argvals[j]);
		}
		else {
			continue;
		}
	}

	/* steal space from first environment entry */
	if (envp[0] != NULL) {
		argvend = envp[0] + strlen (envp[0]);
	}

	/* calculate size we have for process title */
	argsize = (size_t)((char *)argvend - (char *)*argvals - 1);
	memset (*argvals, 0, argsize);

	/* rewrite process title */
	argc = 0;
	snprintf((char *)*argvals, argsize, "jamrouter%d", jamrouter_instance);

	/* signal handlers for clean shutdown */
	init_signal_handlers();

	/* init MIDI system based on selected driver */
	JAMROUTER_DEBUG(DEBUG_CLASS_INIT, "Initializing MIDI:  driver=%s.\n",
	                midi_driver_names[midi_driver]);
	init_sync_info(0, 0);
	init_midi();

	/* initialize JACK audio system based on selected driver */
	snprintf(thread_name, 16, "jamrouter%c-clnt", ('0' + jamrouter_instance));
	pthread_setname_np(pthread_self(), thread_name);
	init_jack_audio();
	while (sample_rate == 0) {
		JAMROUTER_DEBUG(DEBUG_CLASS_INIT,
		                "JACK did not set sample rate.  Re-initializing...\n");
		init_jack_audio();
		usleep(125000);
	}

	/* start the JACK audio system */
	start_jack_audio();

	/* wait for JACK audio to start before starting midi. */
	wait_jack_audio_start();

	snprintf(thread_name, 16, "jamrouter%c-main", ('0' + jamrouter_instance));
	pthread_setname_np(pthread_self(), thread_name);

	/* start MIDI threads */
	start_midi_tx();
	start_midi_rx();

	/* wait until midi threads are ready */
	wait_midi_rx_start();
	wait_midi_tx_start();

	/* debug thread not needed once watchdog is running */
	debug_done = 1;
	pthread_join(debug_thread_p, NULL);

	/* Jamrouter watchdog handles restarting threads on config changes,
	   runs driver supplied watchdog loop iterations, and handles debug
	   output. */
	jamrouter_watchdog();
	stop_midi_tx();
	stop_midi_rx();
	stop_jack_audio();
	output_pending_debug();

	/* Wait for threads created directly by JAMROUTER to terminate. */
	if (midi_rx_thread_p != 0) {
		pthread_join(midi_rx_thread_p,  NULL);
	}
	if (midi_tx_thread_p != 0) {
		pthread_join(midi_tx_thread_p,  NULL);
	}
	output_pending_debug();

	return 0;
}
