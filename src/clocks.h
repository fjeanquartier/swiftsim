/*******************************************************************************
 * This file is part of SWIFT.
 * Copyright (c) 2016 Peter W. Draper (p.w.draper@durham.ac.uk)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 ******************************************************************************/
#ifndef SWIFT_CLOCKS_H
#define SWIFT_CLOCKS_H

#include <sys/times.h>
#include <time.h>
#include "cycle.h"

/* Struct to record a time for the clocks functions. */
struct clocks_time {
#ifdef HAVE_CLOCK_GETTIME
  struct timespec time;
#else
  ticks time;
#endif
};

void clocks_gettime(struct clocks_time *time);
double clocks_diff(struct clocks_time *start, struct clocks_time *end);
const char *clocks_getunit(void);

void clocks_set_cpufreq(unsigned long long freq);
unsigned long long clocks_get_cpufreq(void);
double clocks_from_ticks(ticks tics);
ticks clocks_to_ticks(double interval);
double clocks_diff_ticks(ticks tic, ticks toc);
const char *clocks_get_timesincestart(void);

double clocks_get_cputime_used(void);

#endif /* SWIFT_CLOCKS_H */
