/*******************************************************************************
 * This file is part of SWIFT.
 * Copyright (c) 2012 Pedro Gonnet (pedro.gonnet@durham.ac.uk)
 *                    Matthieu Schaller (matthieu.schaller@durham.ac.uk)
 *               2015 Peter W. Draper (p.w.draper@durham.ac.uk)
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
#ifndef SWIFT_TASK_H
#define SWIFT_TASK_H

#include "../config.h"

/* Includes. */
#include "align.h"
#include "cell.h"
#include "cycle.h"

#define task_align 128

/**
 * @brief The different task types.
 */
enum task_types {
  task_type_none = 0,
  task_type_sort,
  task_type_self,
  task_type_pair,
  task_type_sub_self,
  task_type_sub_pair,
  task_type_init,
  task_type_ghost,
  task_type_extra_ghost,
  task_type_kick,
  task_type_kick_fixdt,
  task_type_send,
  task_type_recv,
  task_type_grav_gather_m,
  task_type_grav_fft,
  task_type_grav_mm,
  task_type_grav_up,
  task_type_grav_external,
  task_type_cooling,
  task_type_sourceterms,
  task_type_count
} __attribute__((packed));

/**
 * @brief The different task sub-types (for pairs, selfs and sub-tasks).
 */
enum task_subtypes {
  task_subtype_none = 0,
  task_subtype_density,
  task_subtype_gradient,
  task_subtype_force,
  task_subtype_grav,
  task_subtype_tend,
  task_subtype_count
} __attribute__((packed));

/**
 * @brief The type of particles/objects this task acts upon in a given cell.
 */
enum task_actions {
  task_action_none,
  task_action_part,
  task_action_gpart,
  task_action_all,
  task_action_multipole,
  task_action_count
};

/**
 * @brief Names of the task types.
 */
extern const char *taskID_names[];

/**
 * @brief Names of the task sub-types.
 */
extern const char *subtaskID_names[];

/**
 * @brief A task to be run by the #scheduler.
 */
struct task {

  /*! Pointers to the cells this task acts upon */
  struct cell *ci, *cj;

  /*! List of tasks unlocked by this one */
  struct task **unlock_tasks;

  /*! Start and end time of this task */
  ticks tic, toc;

#ifdef WITH_MPI

  /*! Buffer for this task's communications */
  void *buff;

  /*! MPI request corresponding to this task */
  MPI_Request req;

#endif

  /*! Flags used to carry additional information (e.g. sort directions) */
  int flags;

  /*! Rank of a task in the order */
  int rank;

  /*! Weight of the task */
  int weight;

  /*! ID of the queue or runner owning this task */
  short int rid;

  /*! Number of tasks unlocked by this one */
  short int nr_unlock_tasks;

  /*! Number of unsatisfied dependencies */
  short int wait;

  /*! Type of the task */
  enum task_types type;

  /*! Sub-type of the task (for the tasks that have one */
  enum task_subtypes subtype;

  /*! Should the scheduler skip this task ? */
  char skip;

  /*! Does this task require the particles to be tightly in the cell ? */
  char tight;

  /*! Is this task implicit (i.e. does not do anything) ? */
  char implicit;

} SWIFT_STRUCT_ALIGN;

/* Function prototypes. */
void task_unlock(struct task *t);
float task_overlap(const struct task *ta, const struct task *tb);
int task_lock(struct task *t);
void task_print_mask(unsigned int mask);
void task_print_submask(unsigned int submask);
void task_do_rewait(struct task *t);

#endif /* SWIFT_TASK_H */
