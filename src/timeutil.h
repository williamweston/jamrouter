/*****************************************************************************
 *
 * timeutil.h
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
#ifndef _JAMROUTER_TIMEUTIL_H_
#define _JAMROUTER_TIMEUTIL_H_

#include <time.h>
#include <sys/types.h>
#ifndef HAVE_CLOCK_GETTIME
#include <sys/time.h>
#endif
#include "jamrouter.h"


#define JAMROUTER_CLOCK_INIT           1111111111

#define TIME_NE                        0x0
#define TIME_EQ                        0x1
#define TIME_GT                        0x2
#define TIME_GE                        0x3
#define TIME_LT                        0x4
#define TIME_LE                        0x5


#if (ARCH_BITS == 32)
typedef float timecalc_t;
#endif /* (ARCH_BITS == 32) */

#if (ARCH_BITS == 64)
typedef double timecalc_t;
#endif /* (ARCH_BITS == 64) */


typedef struct timespec TIMESTAMP;


extern clockid_t            system_clockid;


int timecmp(volatile TIMESTAMP *a,
            volatile TIMESTAMP *b,
            unsigned char mode);
void time_add(volatile TIMESTAMP *a,
              volatile TIMESTAMP *b);
void time_sub(volatile TIMESTAMP *a,
              volatile TIMESTAMP *b);
void time_add_nsecs(volatile TIMESTAMP *a,
                    int nsecs);
void time_sub_nsecs(volatile TIMESTAMP *a,
                    int nsecs);
timecalc_t time_nsecs(volatile TIMESTAMP *a);
timecalc_t time_delta_nsecs(volatile TIMESTAMP *now,
                            volatile TIMESTAMP *start);

void time_copy(volatile TIMESTAMP *dest,
               volatile TIMESTAMP *src);
void time_init(volatile TIMESTAMP *ts,
               long int nsecs);

#ifndef HAVE_CLOCK_GETTIME
int clock_gettime(clockid_t UNUSED(clockid),
                  struct timespec *ts);
#endif
void jamrouter_usleep(int usecs);
void jamrouter_nanosleep(long int nsecs);
void jamrouter_sleep(TIMESTAMP   *request,
                     long int    nsecs,
                     int         usecs,
                     TIMESTAMP   *wake);


#endif /* _JAMROUTER_TIMEUTIL_H_ */
