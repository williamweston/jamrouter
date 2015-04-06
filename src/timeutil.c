/*****************************************************************************
 *
 * timeutil.c
 *
 * JAMRouter:  JACK <--> ALSA MIDI Router
 *
 * Copyright (C) 2015 William Weston <william.h.weston@gmail.com>
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
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <pthread.h>
#include <sys/types.h>
#ifndef HAVE_CLOCK_GETTIME
#include <sys/time.h>
#endif
#include "timeutil.h"


clockid_t               system_clockid        = CLOCK_MONOTONIC;


/*****************************************************************************
 * timecmp()
 *****************************************************************************/
int
timecmp(volatile TIMESTAMP *a, volatile TIMESTAMP *b, unsigned char mode) {
	switch (mode) {
	case TIME_NE:
		return ((a->tv_sec != b->tv_sec) || (a->tv_nsec != b->tv_nsec));
		break;
	case TIME_EQ:
		return ((a->tv_sec == b->tv_sec) && (a->tv_nsec == b->tv_nsec));
		break;
	case TIME_GT:
		return ((a->tv_sec > b->tv_sec) ||
		        ((a->tv_sec == b->tv_sec) && (a->tv_nsec > b->tv_nsec)));
		break;
	case TIME_GE:
		return ((a->tv_sec > b->tv_sec) ||
		        ((a->tv_sec == b->tv_sec) && (a->tv_nsec >= b->tv_nsec)));
		break;
	case TIME_LT:
		return ((a->tv_sec < b->tv_sec) ||
		        ((a->tv_sec == b->tv_sec) && (a->tv_nsec < b->tv_nsec)));
		break;
	case TIME_LE:
		return ((a->tv_sec < b->tv_sec) ||
		        ((a->tv_sec == b->tv_sec) && (a->tv_nsec <= b->tv_nsec)));
		break;
	default:
		return 0;
		break;
	}
}


/*****************************************************************************
 * time_add()
 *****************************************************************************/
void
time_add(volatile TIMESTAMP *a, volatile TIMESTAMP *b) {
	a->tv_sec  += b->tv_sec;
	a->tv_nsec += b->tv_nsec;
	while (a->tv_nsec >= 1000000000) {
		a->tv_nsec -= 1000000000;
		a->tv_sec++;
	}
	while (a->tv_nsec < 0) {
		a->tv_nsec += 1000000000;
		a->tv_sec--;
	}
}


/*****************************************************************************
 * time_sub()
 *****************************************************************************/
void
time_sub(volatile TIMESTAMP *a, volatile TIMESTAMP *b) {
	a->tv_sec -= b->tv_sec;
	if (b->tv_nsec > a->tv_nsec) {
		a->tv_nsec = a->tv_nsec + 1000000000 - b->tv_nsec;
		a->tv_sec--;
	}
	else {
		a->tv_nsec -= b->tv_nsec;
	}
	while (a->tv_nsec >= 1000000000) {
		a->tv_nsec -= 1000000000;
		a->tv_sec++;
	}
	while (a->tv_nsec < 0) {
		a->tv_nsec += 1000000000;
		a->tv_sec--;
	}
}


/*****************************************************************************
 * time_add_nsecs()
 *****************************************************************************/
void
time_add_nsecs(volatile TIMESTAMP *a, int nsecs) {
	int nsec = nsecs;

	while (nsec > 1000000000) {
		nsec -= 1000000000;
		a->tv_sec++;
	}
	while (nsec < 0) {
		nsec += 1000000000;
		a->tv_sec--;
	}
	a->tv_nsec += nsec;
	while (a->tv_nsec >= 1000000000) {
		a->tv_nsec -= 1000000000;
		a->tv_sec++;
	}
	while (a->tv_nsec < 0) {
		a->tv_nsec += 1000000000;
		a->tv_sec--;
	}
}


/*****************************************************************************
 * time_sub_nsecs()
 *****************************************************************************/
void
time_sub_nsecs(volatile TIMESTAMP *a, int nsecs) {
	int nsec = nsecs;

	while (nsec > 1000000000) {
		nsec -= 1000000000;
		a->tv_sec--;
	}
	while (nsec < 0) {
		nsec += 1000000000;
		a->tv_sec++;
	}
	if (a->tv_nsec < nsec) {
		a->tv_nsec = a->tv_nsec + 1000000000 - nsec;
		a->tv_sec--;
	}
	else {
		a->tv_nsec -= nsec;
	}
	while (a->tv_nsec >= 1000000000) {
		a->tv_nsec -= 1000000000;
		a->tv_sec++;
	}
	while (a->tv_nsec < 0) {
		a->tv_nsec += 1000000000;
		a->tv_sec--;
	}
}


/*****************************************************************************
 * time_nsecs()
 *****************************************************************************/
timecalc_t
time_nsecs(volatile TIMESTAMP *a)
{
	return (((timecalc_t)(a->tv_sec) * (timecalc_t)(1000000000)) +
	        (timecalc_t)(a->tv_nsec));
}


/*****************************************************************************
 * time_copy()
 *****************************************************************************/
TIMESTAMP *
time_copy(volatile TIMESTAMP *dest, volatile TIMESTAMP *src)
{
	dest->tv_sec  = src->tv_sec;
	dest->tv_nsec = src->tv_nsec;

	return (TIMESTAMP *)dest;
}


/*****************************************************************************
 * time_init()
 *****************************************************************************/
void
time_init(volatile TIMESTAMP *ts, long int nsecs)
{
	ts->tv_sec  = 0;
	ts->tv_nsec = nsecs;
}


/*****************************************************************************
 * clock_gettime()
 *
 * replacement for systems without it.
 *****************************************************************************/
#ifndef HAVE_CLOCK_GETTIME
int
clock_gettime(clockid_t UNUSED(clockid), struct timespec *ts)
{
	struct timeval  tv;

	if (gettimeofday(&tv, NULL) == 0) {
		ts->tv_sec  = tv.tv_sec;
		ts->tv_nsec = tv.tv_usec * 1000;
		return 0;
	}
	return -1;
}
#endif /* !HAVE_CLOCK_GETTIME */


/*****************************************************************************
 * jamrouter_usleep()
 *****************************************************************************/
void
jamrouter_usleep(int usecs)
{
#ifdef HAVE_CLOCK_NANOSLEEP
	struct timespec         sleep_time       = { 0, usecs * 1000 };
#endif

	if (usecs > 0) {
#ifdef HAVE_CLOCK_NANOSLEEP
		clock_nanosleep(CLOCK_MONOTONIC, 0, &sleep_time, NULL);
#else
		usleep(sleep_time);
#endif
	}
}


/*****************************************************************************
 * jamrouter_nanosleep()
 *****************************************************************************/
void
jamrouter_nanosleep(long int nsecs)
{
#ifdef HAVE_CLOCK_NANOSLEEP
	struct timespec         sleep_time       = { 0, nsecs };
#endif

	if (nsecs > 5000) {
#ifdef HAVE_CLOCK_NANOSLEEP
		clock_nanosleep(CLOCK_MONOTONIC, 0, &sleep_time, NULL);
#else
		nanosleep(sleep_time);
#endif
	}
}


/*****************************************************************************
 * jamrouter_sleep()
 *
 * TODO: implement realtime signal handling so that this can be completed.
 *****************************************************************************/
void
jamrouter_sleep(TIMESTAMP   *request,
                long int    nsecs,
                int         usecs,
                TIMESTAMP   *wake) {
#ifdef HAVE_CLOCK_NANOSLEEP
	struct timespec         sleep_time       = { 0, nsecs + (usecs * 1000) };
	struct timespec         remain           = { 0, 0 };
#endif

	if (request != NULL) {
		time_copy(&sleep_time, request);
	}

	if ((sleep_time.tv_sec > 0) || (sleep_time.tv_nsec > 5000)) {
#ifdef HAVE_CLOCK_NANOSLEEP
		while ((remain.tv_sec > 0) || (remain.tv_nsec > 5000)) {
			clock_nanosleep(CLOCK_MONOTONIC, 0, &sleep_time, &remain);
		}
		if (wake != NULL) {
			time_copy(wake, &sleep_time);
			time_sub(wake, &remain);
		}
#else
		nanosleep(sleep_time);
#endif
	}
}
