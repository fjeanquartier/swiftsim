/*******************************************************************************
 * This file is part of SWIFT.
 * Copyright (c) 2012 Pedro Gonnet (pedro.gonnet@durham.ac.uk)
 *                    Matthieu Schaller (matthieu.schaller@durham.ac.uk)
 *               2015 Peter W. Draper (p.w.draper@durham.ac.uk)
 *                    Angus Lepper (angus.lepper@ed.ac.uk)
 *               2016 John A. Regan (john.a.regan@durham.ac.uk)
 *                    Tom Theuns (tom.theuns@durham.ac.uk)
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
#ifndef SWIFT_ENGINE_H
#define SWIFT_ENGINE_H

/* Config parameters. */
#include "../config.h"

/* MPI headers. */
#ifdef WITH_MPI
#include <mpi.h>
#endif

/* Some standard headers. */
#include <pthread.h>
#include <stdio.h>

/* Includes. */
#include "clocks.h"
#include "cooling_struct.h"
#include "parser.h"
#include "partition.h"
#include "potential.h"
#include "runner.h"
#include "scheduler.h"
#include "space.h"
#include "task.h"
#include "units.h"

/* Some constants. */
enum engine_policy {
  engine_policy_none = 0,
  engine_policy_rand = (1 << 0),
  engine_policy_steal = (1 << 1),
  engine_policy_keep = (1 << 2),
  engine_policy_block = (1 << 3),
  engine_policy_fixdt = (1 << 4),
  engine_policy_cputight = (1 << 5),
  engine_policy_mpi = (1 << 6),
  engine_policy_setaffinity = (1 << 7),
  engine_policy_hydro = (1 << 8),
  engine_policy_self_gravity = (1 << 9),
  engine_policy_external_gravity = (1 << 10),
  engine_policy_cosmology = (1 << 11),
  engine_policy_drift_all = (1 << 12),
  engine_policy_cooling = (1 << 13),
};

extern const char *engine_policy_names[];

#define engine_queue_scale 1.2
#define engine_maxtaskspercell 96
#define engine_maxproxies 64
#define engine_tasksreweight 10
#define engine_parts_size_grow 1.05
#define engine_redistribute_alloc_margin 1.2
#define engine_default_energy_file_name "energy"
#define engine_default_timesteps_file_name "timesteps"

/* The rank of the engine as a global variable (for messages). */
extern int engine_rank;

/* The maximal number of timesteps in a simulation */
#define max_nr_timesteps (1 << 28)

/* Data structure for the engine. */
struct engine {

  /* Number of threads on which to run. */
  int nr_threads;

  /* The space with which the runner is associated. */
  struct space *s;

  /* The runner's threads. */
  struct runner *runners;

  /* The running policy. */
  int policy;

  /* The task scheduler. */
  struct scheduler sched;

  /* Common threadpool for all the engine's tasks. */
  struct threadpool threadpool;

  /* The minimum and maximum allowed dt */
  double dt_min, dt_max;

  /* Time of the simulation beginning */
  double timeBegin;

  /* Time of the simulation end */
  double timeEnd;

  /* The previous system time. */
  double timeOld;
  int ti_old;

  /* The current system time. */
  double time;
  int ti_current;

  /* Time step */
  double timeStep;

  /* Time base */
  double timeBase;
  double timeBase_inv;

  /* Minimal ti_end for the next time-step */
  int ti_end_min;

  /* Are we drifting all particles now ? */
  int drift_all;

  /* Number of particles updated */
  size_t updates, g_updates;

  /* The internal system of units */
  const struct UnitSystem *internalUnits;

  /* Snapshot information */
  double timeFirstSnapshot;
  double deltaTimeSnapshot;
  int ti_nextSnapshot;
  char snapshotBaseName[200];
  int snapshotCompression;
  struct UnitSystem *snapshotUnits;

  /* Statistics information */
  FILE *file_stats;
  double timeLastStatistics;
  double deltaTimeStatistics;

  /* Timesteps information */
  FILE *file_timesteps;

  /* The current step number. */
  int step;

  /* The number of particles updated in the previous step. */
  int count_step;

  /* Data for the threads' barrier. */
  pthread_mutex_t barrier_mutex;
  pthread_cond_t barrier_cond;
  volatile int barrier_running, barrier_launch, barrier_launchcount;

  /* ID of the node this engine lives on. */
  int nr_nodes, nodeID;

  /* Proxies for the other nodes in this simulation. */
  struct proxy *proxies;
  int nr_proxies, *proxy_ind;

  /* Tic/toc at the start/end of a step. */
  ticks tic_step, toc_step;

  /* Wallclock time of the last time-step */
  float wallclock_time;

  /* Force the engine to rebuild? */
  int forcerebuild;
  int forcerepart;

  /* Repartitioning data. */
  enum repartition_type reparttype;
  struct repartition_data repartdata;

  /* How many steps have we done with the same set of tasks? */
  int tasks_age;

  /* Linked list for cell-task association. */
  struct link *links;
  int nr_links, size_links;

  /* Are we talkative ? */
  int verbose;

  /* Physical constants definition */
  const struct phys_const *physical_constants;

  /* Properties of the hydro scheme */
  const struct hydro_props *hydro_properties;

  /* Properties of external gravitational potential */
  const struct external_potential *external_potential;

  /* Properties of the cooling scheme */
  const struct cooling_function_data *cooling_func;

  /* The (parsed) parameter file */
  const struct swift_params *parameter_file;

};

/* Function prototypes. */
void engine_barrier(struct engine *e, int tid);
void engine_compute_next_snapshot_time(struct engine *e);
void engine_drift(struct engine *e);
void engine_dump_snapshot(struct engine *e);
void engine_init(struct engine *e, struct space *s,
                 const struct swift_params *params, int nr_nodes, int nodeID,
                 int nr_threads, int with_aff, int policy, int verbose,
                 enum repartition_type reparttype,
                 const struct UnitSystem *internal_units,
                 const struct phys_const *physical_constants,
                 const struct hydro_props *hydro,
                 const struct external_potential *potential,
                 const struct cooling_function_data *cooling);
void engine_launch(struct engine *e, int nr_runners, unsigned int mask,
                   unsigned int submask);
void engine_prepare(struct engine *e, int nodrift);
void engine_print(struct engine *e);
void engine_init_particles(struct engine *e, int flag_entropy_ICs);
void engine_step(struct engine *e);
void engine_maketasks(struct engine *e);
void engine_split(struct engine *e, struct partition *initial_partition);
void engine_exchange_strays(struct engine *e, size_t offset_parts,
                            int *ind_part, size_t *Npart, size_t offset_gparts,
                            int *ind_gpart, size_t *Ngpart);
void engine_rebuild(struct engine *e);
void engine_repartition(struct engine *e);
void engine_makeproxies(struct engine *e);
void engine_redistribute(struct engine *e);
void engine_print_policy(struct engine *e);
int engine_is_done(struct engine *e);
void engine_pin();
void engine_unpin();
void engine_clean(struct engine *e);

#endif /* SWIFT_ENGINE_H */
