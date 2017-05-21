#!/usr/bin/env python
"""
Usage:
    plot_tasks_MPI.py [options] input.dat png-output-prefix

where input.dat is a thread info file for a step.  Use the '-y interval' flag
of the swift MPI command to create these. The output plot will be called
'png-output-prefix<mpi-rank>.png', i.e. one each for all the threads in each
MPI rank.  The --limit option can be used to produce plots with the same time
span and the --expand option to expand each thread line into '*expand' lines,
so that adjacent tasks of the same type can be distinguished. Other options
can be seen using the --help flag.

See the command 'process_plot_tasks_MPI' to efficiently wrap this command to
process a number of thread info files and create an HTML file to view them.

This file is part of SWIFT.

Copyright (C) 2015 Pedro Gonnet (pedro.gonnet@durham.ac.uk),
                   Bert Vandenbroucke (bert.vandenbroucke@ugent.be)
                   Matthieu Schaller (matthieu.schaller@durham.ac.uk)
          (C) 2017 Peter W. Draper (p.w.draper@durham.ac.uk)
All Rights Reserved.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published
by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
"""

import matplotlib
matplotlib.use("Agg")
import matplotlib.collections as collections
import matplotlib.ticker as plticker
import pylab as pl
import sys
import argparse

#  Handle the command line.
parser = argparse.ArgumentParser(description="Plot task graphs")

parser.add_argument("input", help="Thread data file (-y output)")
parser.add_argument("outbase", help="Base name for output graphic files (PNG)")
parser.add_argument("-l", "--limit", dest="limit",
                    help="Upper time limit in millisecs (def: depends on data)",
                    default=0, type=int)
parser.add_argument("-e", "--expand", dest="expand",
                    help="Thread expansion factor (def: 1)",
                    default=1, type=int)
parser.add_argument("--height", dest="height",
                    help="Height of plot in inches (def: 4)",
                    default=4., type=float)
parser.add_argument("--width", dest="width",
                    help="Width of plot in inches (def: 16)",
                    default=16., type=float)
parser.add_argument("--nolegend", dest="nolegend",
                    help="Whether to show the legend (def: False)",
                    default=False, action="store_true")
parser.add_argument("-v", "--verbose", dest="verbose",
                    help="Show colour assignments and other details (def: False)",
                    default=False, action="store_true")

args = parser.parse_args()
infile = args.input
outbase = args.outbase
delta_t = args.limit
expand = args.expand

#  Basic plot configuration.
PLOT_PARAMS = {"axes.labelsize": 10,
               "axes.titlesize": 10,
               "font.size": 12,
               "legend.fontsize": 12,
               "xtick.labelsize": 10,
               "ytick.labelsize": 10,
               "figure.figsize" : (args.width, args.height),
               "figure.subplot.left" : 0.03,
               "figure.subplot.right" : 0.995,
               "figure.subplot.bottom" : 0.1,
               "figure.subplot.top" : 0.99,
               "figure.subplot.wspace" : 0.,
               "figure.subplot.hspace" : 0.,
               "lines.markersize" : 6,
               "lines.linewidth" : 3.
               }
pl.rcParams.update(PLOT_PARAMS)

#  Tasks and subtypes. Indexed as in tasks.h.
TASKTYPES = ["none", "sort", "self", "pair", "sub_self", "sub_pair",
             "init_grav", "ghost", "extra_ghost", "drift", "kick1", "kick2",
             "timestep", "send", "recv", "grav_top_level", "grav_long_range",
             "grav_mm", "grav_down", "cooling", "sourceterms", "count"]

SUBTYPES = ["none", "density", "gradient", "force", "grav", "external_grav",
            "tend", "xv", "rho", "gpart", "multipole", "spart", "count"]

#  Task/subtypes of interest.
FULLTYPES = ["self/force", "self/density", "self/grav", "sub_self/force",
             "sub_self/density", "pair/force", "pair/density", "pair/grav",
             "sub_pair/force",
             "sub_pair/density", "recv/xv", "send/xv", "recv/rho", "send/rho",
             "recv/tend", "send/tend"]

#  A number of colours for the various types. Recycled when there are
#  more task types than colours...

colours = ["cyan", "lightgray", "darkblue", "yellow", "tan", "dodgerblue",
           "sienna", "aquamarine", "bisque", "blue", "green", "brown",
           "purple", "mocassin", "olivedrab", "chartreuse", "darksage",
           "darkgreen", "green", "mediumseagreen", "mediumaquamarine",
           "darkslategrey", "mediumturquoise", "black", "cadetblue", "skyblue",
           "red", "slategray", "gold", "slateblue", "blueviolet",
           "mediumorchid", "firebrick", "magenta", "hotpink", "pink",
           "orange", "lightgreen"]
maxcolours = len(colours)

#  Set colours of task/subtype.
TASKCOLOURS = {}
ncolours = 0
for task in TASKTYPES:
    TASKCOLOURS[task] = colours[ncolours]
    ncolours = (ncolours + 1) % maxcolours

SUBCOLOURS = {}
for task in FULLTYPES:
    SUBCOLOURS[task] = colours[ncolours]
    ncolours = (ncolours + 1) % maxcolours

for task in SUBTYPES:
    SUBCOLOURS[task] = colours[ncolours]
    ncolours = (ncolours + 1) % maxcolours

#  For fiddling with colours...
if args.verbose:
    print "#Selected colours:"
    for task in TASKCOLOURS.keys():
        print "# " + task + ": " + TASKCOLOURS[task]
    for task in SUBCOLOURS.keys():
        print "# " + task + ": " + SUBCOLOURS[task]

#  Read input.
data = pl.loadtxt( infile )

#  Get CPU_CLOCK to convert ticks into milliseconds.
full_step = data[0,:]
CPU_CLOCK = float(full_step[-1]) / 1000.0
if args.verbose:
    print "CPU frequency:", CPU_CLOCK * 1000.0

nranks = int(max(data[:,0])) + 1
print "Number of ranks:", nranks
nthread = int(max(data[:,1])) + 1
print "Number of threads:", nthread

# Avoid start and end times of zero.
sdata = data[data[:,5] != 0]
sdata = sdata[sdata[:,6] != 0]

# Each rank can have different clock (compute node), but we want to use the
# same delta times range for comparisons, so we suck it up and take the hit of
# precalculating this, unless the user knows better.
delta_t = delta_t * CPU_CLOCK
if delta_t == 0:
    for rank in range(nranks):
        data = sdata[sdata[:,0] == rank]
        dt = max(data[:,6]) - min(data[:,5])
        if dt > delta_t:
            delta_t = dt
    print "Data range: ", delta_t / CPU_CLOCK, "ms"


# Once more doing the real gather and plots this time.
for rank in range(nranks):
    data = sdata[sdata[:,0] == rank]

    #  Start and end times for this rank.
    full_step = data[0,:]
    tic_step = int(full_step[5])
    toc_step = int(full_step[6])
    data = data[1:,:]

    #  Dummy image for ranks that have no tasks.
    if data.size == 0:
        print "rank ", rank, " has no tasks"
        fig = pl.figure()
        ax = fig.add_subplot(1,1,1)
        ax.set_xlim(-delta_t * 0.01 / CPU_CLOCK, delta_t * 1.01 / CPU_CLOCK)
        ax.set_ylim(0, nthread*expand)
        start_t = tic_step
        end_t = (toc_step - start_t) / CPU_CLOCK
    else:

        start_t = float(tic_step)
        data[:,5] -= start_t
        data[:,6] -= start_t
        end_t = (toc_step - start_t) / CPU_CLOCK

        tasks = {}
        tasks[-1] = []
        for i in range(nthread*expand):
            tasks[i] = []

        # Counters for each thread when expanding.
        ecounter = []
        for i in range(nthread):
            ecounter.append(0)

        num_lines = pl.shape(data)[0]
        for line in range(num_lines):
            thread = int(data[line,1])

            # Expand to cover extra lines if expanding.
            ethread = thread * expand + (ecounter[thread] % expand)
            ecounter[thread] = ecounter[thread] + 1
            thread = ethread

            tasks[thread].append({})
            tasktype = TASKTYPES[int(data[line,2])]
            subtype = SUBTYPES[int(data[line,3])]
            tasks[thread][-1]["type"] = tasktype
            tasks[thread][-1]["subtype"] = subtype
            tic = int(data[line,5]) / CPU_CLOCK
            toc = int(data[line,6]) / CPU_CLOCK
            tasks[thread][-1]["tic"] = tic
            tasks[thread][-1]["toc"] = toc
            if "self" in tasktype or "pair" in tasktype or "recv" in tasktype or "send" in tasktype:
                fulltype = tasktype + "/" + subtype
                if fulltype in SUBCOLOURS:
                    tasks[thread][-1]["colour"] = SUBCOLOURS[fulltype]
                else:
                    tasks[thread][-1]["colour"] = SUBCOLOURS[subtype]
            else:
                tasks[thread][-1]["colour"] = TASKCOLOURS[tasktype]

        # Use expanded threads from now on.
        nethread = nthread * expand

        typesseen = []
        fig = pl.figure()
        ax = fig.add_subplot(1,1,1)
        ax.set_xlim(-delta_t * 0.01 / CPU_CLOCK, delta_t * 1.01 / CPU_CLOCK)
        ax.set_ylim(0, nethread)
        for i in range(nethread):

            #  Collect ranges and colours into arrays.
            tictocs = []
            colours = []
            j = 0
            for task in tasks[i]:
                tictocs.append((task["tic"], task["toc"] - task["tic"]))
                colours.append(task["colour"])

                #  Legend support, collections don't add to this.
                if task["subtype"] != "none":
                    qtask = task["type"] + "/" + task["subtype"]
                else:
                    qtask = task["type"]

                if qtask not in typesseen:
                    pl.plot([], [], color=task["colour"], label=qtask)
                    typesseen.append(qtask)

            #  Now plot.
            ax.broken_barh(tictocs, [i+0.05,0.90], facecolors = colours, linewidth=0)


    #  Legend and room for it.
    nrow = len(typesseen) / 5
    if len(typesseen) * 5 < nrow:
        nrow = nrow + 1
    ax.fill_between([0, 0], nethread+0.5, nethread + nrow + 0.5, facecolor="white")
    ax.set_ylim(0, nethread + nrow + 1)
    if data.size > 0:
        ax.legend(loc=1, shadow=True, mode="expand", ncol=5)

    # Start and end of time-step
    ax.plot([0, 0], [0, nethread + nrow + 1], 'k--', linewidth=1)
    ax.plot([end_t, end_t], [0, nethread + nrow + 1], 'k--', linewidth=1)

    ax.set_xlabel("Wall clock time [ms]")

    if expand == 1:
        ax.set_ylabel("Thread ID" )
    else:
        ax.set_ylabel("Thread ID * " + str(expand) )
    ax.set_yticks(pl.array(range(nethread)), True)

    loc = plticker.MultipleLocator(base=expand)
    ax.yaxis.set_major_locator(loc)
    ax.grid(True, which='major', axis="y", linestyle="-")

    pl.show()
    outpng = outbase + str(rank) + ".png"
    pl.savefig(outpng)
    print "Graphics done, output written to", outpng

sys.exit(0)
