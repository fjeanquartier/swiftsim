/*******************************************************************************
 * This file is part of SWIFT.
 * Copyright (c) 2012 Pedro Gonnet (pedro.gonnet@durham.ac.uk)
 *                    Matthieu Schaller (matthieu.schaller@durham.ac.uk)
 *               2016 Peter W. Draper (p.w.draper@durham.ac.uk)
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

/* Config parameters. */
#include "../config.h"

/* Some standard headers. */
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* MPI headers. */
#ifdef WITH_MPI
#include <mpi.h>
#endif

/* This object's header. */
#include "scheduler.h"

/* Local headers. */
#include "atomic.h"
#include "const.h"
#include "cycle.h"
#include "error.h"
#include "intrinsics.h"
#include "kernel_hydro.h"
#include "queue.h"
#include "space.h"
#include "task.h"
#include "timers.h"

/**
 * @brief Add an unlock_task to the given task.
 *
 * @param s The #scheduler.
 * @param ta The unlocking #task.
 * @param tb The #task that will be unlocked.
 */

void scheduler_addunlock(struct scheduler *s, struct task *ta,
                         struct task *tb) {
  /* Get an index at which to store this unlock. */
  const int ind = atomic_inc(&s->nr_unlocks);

  /* Does the buffer need to be grown? */
  if (ind == s->size_unlocks) {
    /* Allocate the new buffer. */
    struct task **unlocks_new;
    int *unlock_ind_new;
    const int size_unlocks_new = s->size_unlocks * 2;
    if ((unlocks_new = (struct task **)malloc(sizeof(struct task *) *
                                              size_unlocks_new)) == NULL ||
        (unlock_ind_new = (int *)malloc(sizeof(int) * size_unlocks_new)) ==
            NULL)
      error("Failed to re-allocate unlocks.");

    /* Wait for all writes to the old buffer to complete. */
    while (s->completed_unlock_writes < ind)
      ;

    /* Copy the buffers. */
    memcpy(unlocks_new, s->unlocks, sizeof(struct task *) * ind);
    memcpy(unlock_ind_new, s->unlock_ind, sizeof(int) * ind);
    free(s->unlocks);
    free(s->unlock_ind);
    s->unlocks = unlocks_new;
    s->unlock_ind = unlock_ind_new;

    /* Publish the new buffer size. */
    s->size_unlocks = size_unlocks_new;
  }

  /* Wait for there to actually be space at my index. */
  while (ind > s->size_unlocks)
    ;

  /* Write the unlock to the scheduler. */
  s->unlocks[ind] = tb;
  s->unlock_ind[ind] = ta - s->tasks;
  atomic_inc(&s->completed_unlock_writes);
}

/**
 * @brief Split a task if too large.
 *
 * @param t The #task
 * @param s The #scheduler we are working in.
 */

static void scheduler_splittask(struct task *t, struct scheduler *s) {

  /* Static constants. */
  static const int pts[7][8] = {
      {-1, 12, 10, 9, 4, 3, 1, 0},     {-1, -1, 11, 10, 5, 4, 2, 1},
      {-1, -1, -1, 12, 7, 6, 4, 3},    {-1, -1, -1, -1, 8, 7, 5, 4},
      {-1, -1, -1, -1, -1, 12, 10, 9}, {-1, -1, -1, -1, -1, -1, 11, 10},
      {-1, -1, -1, -1, -1, -1, -1, 12}};
  static const float sid_scale[13] = {
      0.1897f, 0.4025f, 0.1897f, 0.4025f, 0.5788f, 0.4025f, 0.1897f,
      0.4025f, 0.1897f, 0.4025f, 0.5788f, 0.4025f, 0.5788f};

  /* Iterate on this task until we're done with it. */
  int redo = 1;
  while (redo) {

    /* Reset the redo flag. */
    redo = 0;

    /* Non-splittable task? */
    if ((t->ci == NULL || (t->type == task_type_pair && t->cj == NULL)) ||
        ((t->type == task_type_kick) && t->ci->nodeID != s->nodeID) ||
        ((t->type == task_type_init) && t->ci->nodeID != s->nodeID)) {
      t->type = task_type_none;
      t->skip = 1;
      break;
    }

    /* Self-interaction? */
    if (t->type == task_type_self) {

      /* Get a handle on the cell involved. */
      struct cell *ci = t->ci;
      const double hi = ci->dmin;

      /* Foreign task? */
      if (ci->nodeID != s->nodeID) {
        t->skip = 1;
        break;
      }

      /* Is this cell even split? */
      if (ci->split && ci->h_max * kernel_gamma * space_stretch < hi / 2) {

        /* Make a sub? */
        if (scheduler_dosub &&
            ((ci->count > 0 && ci->count < space_subsize / ci->count) ||
             (ci->gcount > 0 && ci->gcount < space_subsize / ci->gcount))) {

          /* convert to a self-subtask. */
          t->type = task_type_sub_self;

          /* Otherwise, make tasks explicitly. */
        } else {

          /* Take a step back (we're going to recycle the current task)... */
          redo = 1;

          /* Add the self tasks. */
          int first_child = 0;
          while (ci->progeny[first_child] == NULL) first_child++;
          t->ci = ci->progeny[first_child];
          for (int k = first_child + 1; k < 8; k++)
            if (ci->progeny[k] != NULL)
              scheduler_splittask(
                  scheduler_addtask(s, task_type_self, t->subtype, 0, 0,
                                    ci->progeny[k], NULL, 0),
                  s);

          /* Make a task for each pair of progeny. */
          for (int j = 0; j < 8; j++)
            if (ci->progeny[j] != NULL)
              for (int k = j + 1; k < 8; k++)
                if (ci->progeny[k] != NULL)
                  scheduler_splittask(
                      scheduler_addtask(s, task_type_pair, t->subtype,
                                        pts[j][k], 0, ci->progeny[j],
                                        ci->progeny[k], 0),
                      s);
        }
      }

      /* Pair interaction? */
    } else if (t->type == task_type_pair && t->subtype != task_subtype_grav) {

      /* Get a handle on the cells involved. */
      struct cell *ci = t->ci;
      struct cell *cj = t->cj;
      const double hi = ci->dmin;
      const double hj = cj->dmin;

      /* Foreign task? */
      if (ci->nodeID != s->nodeID && cj->nodeID != s->nodeID) {
        t->skip = 1;
        break;
      }

      /* Get the sort ID, use space_getsid and not t->flags
         to make sure we get ci and cj swapped if needed. */
      double shift[3];
      int sid = space_getsid(s->space, &ci, &cj, shift);

      /* Should this task be split-up? */
      if (ci->split && cj->split &&
          ci->h_max * kernel_gamma * space_stretch < hi / 2 &&
          cj->h_max * kernel_gamma * space_stretch < hj / 2) {

        /* Replace by a single sub-task? */
        if (scheduler_dosub &&
            ci->count * sid_scale[sid] < space_subsize / cj->count &&
            sid != 0 && sid != 2 && sid != 6 && sid != 8) {

          /* Make this task a sub task. */
          t->type = task_type_sub_pair;

          /* Otherwise, split it. */
        } else {

          /* Take a step back (we're going to recycle the current task)... */
          redo = 1;

          /* For each different sorting type... */
          switch (sid) {

            case 0: /* (  1 ,  1 ,  1 ) */
              t->ci = ci->progeny[7];
              t->cj = cj->progeny[0];
              t->flags = 0;
              break;

            case 1: /* (  1 ,  1 ,  0 ) */
              t->ci = ci->progeny[6];
              t->cj = cj->progeny[0];
              t->flags = 1;
              t->tight = 1;
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 1, 0,
                                    ci->progeny[7], cj->progeny[1], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 0, 0,
                                    ci->progeny[6], cj->progeny[1], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 2, 0,
                                    ci->progeny[7], cj->progeny[0], 1),
                  s);
              break;

            case 2: /* (  1 ,  1 , -1 ) */
              t->ci = ci->progeny[6];
              t->cj = cj->progeny[1];
              t->flags = 2;
              t->tight = 1;
              break;

            case 3: /* (  1 ,  0 ,  1 ) */
              t->ci = ci->progeny[5];
              t->cj = cj->progeny[0];
              t->flags = 3;
              t->tight = 1;
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 3, 0,
                                    ci->progeny[7], cj->progeny[2], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 0, 0,
                                    ci->progeny[5], cj->progeny[2], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 6, 0,
                                    ci->progeny[7], cj->progeny[0], 1),
                  s);
              break;

            case 4: /* (  1 ,  0 ,  0 ) */
              t->ci = ci->progeny[4];
              t->cj = cj->progeny[0];
              t->flags = 4;
              t->tight = 1;
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 5, 0,
                                    ci->progeny[5], cj->progeny[0], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 7, 0,
                                    ci->progeny[6], cj->progeny[0], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 8, 0,
                                    ci->progeny[7], cj->progeny[0], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 3, 0,
                                    ci->progeny[4], cj->progeny[1], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 4, 0,
                                    ci->progeny[5], cj->progeny[1], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 6, 0,
                                    ci->progeny[6], cj->progeny[1], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 7, 0,
                                    ci->progeny[7], cj->progeny[1], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 1, 0,
                                    ci->progeny[4], cj->progeny[2], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 2, 0,
                                    ci->progeny[5], cj->progeny[2], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 4, 0,
                                    ci->progeny[6], cj->progeny[2], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 5, 0,
                                    ci->progeny[7], cj->progeny[2], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 0, 0,
                                    ci->progeny[4], cj->progeny[3], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 1, 0,
                                    ci->progeny[5], cj->progeny[3], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 3, 0,
                                    ci->progeny[6], cj->progeny[3], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 4, 0,
                                    ci->progeny[7], cj->progeny[3], 1),
                  s);
              break;

            case 5: /* (  1 ,  0 , -1 ) */
              t->ci = ci->progeny[4];
              t->cj = cj->progeny[1];
              t->flags = 5;
              t->tight = 1;
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 5, 0,
                                    ci->progeny[6], cj->progeny[3], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 2, 0,
                                    ci->progeny[4], cj->progeny[3], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 8, 0,
                                    ci->progeny[6], cj->progeny[1], 1),
                  s);
              break;

            case 6: /* (  1 , -1 ,  1 ) */
              t->ci = ci->progeny[5];
              t->cj = cj->progeny[2];
              t->flags = 6;
              t->tight = 1;
              break;

            case 7: /* (  1 , -1 ,  0 ) */
              t->ci = ci->progeny[4];
              t->cj = cj->progeny[3];
              t->flags = 6;
              t->tight = 1;
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 8, 0,
                                    ci->progeny[5], cj->progeny[2], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 7, 0,
                                    ci->progeny[4], cj->progeny[2], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 7, 0,
                                    ci->progeny[5], cj->progeny[3], 1),
                  s);
              break;

            case 8: /* (  1 , -1 , -1 ) */
              t->ci = ci->progeny[4];
              t->cj = cj->progeny[3];
              t->flags = 8;
              t->tight = 1;
              break;

            case 9: /* (  0 ,  1 ,  1 ) */
              t->ci = ci->progeny[3];
              t->cj = cj->progeny[0];
              t->flags = 9;
              t->tight = 1;
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 9, 0,
                                    ci->progeny[7], cj->progeny[4], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 0, 0,
                                    ci->progeny[3], cj->progeny[4], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 8, 0,
                                    ci->progeny[7], cj->progeny[0], 1),
                  s);
              break;

            case 10: /* (  0 ,  1 ,  0 ) */
              t->ci = ci->progeny[2];
              t->cj = cj->progeny[0];
              t->flags = 10;
              t->tight = 1;
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 11, 0,
                                    ci->progeny[3], cj->progeny[0], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 7, 0,
                                    ci->progeny[6], cj->progeny[0], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 6, 0,
                                    ci->progeny[7], cj->progeny[0], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 9, 0,
                                    ci->progeny[2], cj->progeny[1], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 10, 0,
                                    ci->progeny[3], cj->progeny[1], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 8, 0,
                                    ci->progeny[6], cj->progeny[1], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 7, 0,
                                    ci->progeny[7], cj->progeny[1], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 1, 0,
                                    ci->progeny[2], cj->progeny[4], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 2, 0,
                                    ci->progeny[3], cj->progeny[4], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 10, 0,
                                    ci->progeny[6], cj->progeny[4], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 11, 0,
                                    ci->progeny[7], cj->progeny[4], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 0, 0,
                                    ci->progeny[2], cj->progeny[5], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 1, 0,
                                    ci->progeny[3], cj->progeny[5], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 9, 0,
                                    ci->progeny[6], cj->progeny[5], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 10, 0,
                                    ci->progeny[7], cj->progeny[5], 1),
                  s);
              break;

            case 11: /* (  0 ,  1 , -1 ) */
              t->ci = ci->progeny[2];
              t->cj = cj->progeny[1];
              t->flags = 11;
              t->tight = 1;
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 11, 0,
                                    ci->progeny[6], cj->progeny[5], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 2, 0,
                                    ci->progeny[2], cj->progeny[5], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 6, 0,
                                    ci->progeny[6], cj->progeny[1], 1),
                  s);
              break;

            case 12: /* (  0 ,  0 ,  1 ) */
              t->ci = ci->progeny[1];
              t->cj = cj->progeny[0];
              t->flags = 12;
              t->tight = 1;
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 11, 0,
                                    ci->progeny[3], cj->progeny[0], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 5, 0,
                                    ci->progeny[5], cj->progeny[0], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 2, 0,
                                    ci->progeny[7], cj->progeny[0], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 9, 0,
                                    ci->progeny[1], cj->progeny[2], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 12, 0,
                                    ci->progeny[3], cj->progeny[2], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 8, 0,
                                    ci->progeny[5], cj->progeny[2], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 5, 0,
                                    ci->progeny[7], cj->progeny[2], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 3, 0,
                                    ci->progeny[1], cj->progeny[4], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 6, 0,
                                    ci->progeny[3], cj->progeny[4], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 12, 0,
                                    ci->progeny[5], cj->progeny[4], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 11, 0,
                                    ci->progeny[7], cj->progeny[4], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 0, 0,
                                    ci->progeny[1], cj->progeny[6], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 3, 0,
                                    ci->progeny[3], cj->progeny[6], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 9, 0,
                                    ci->progeny[5], cj->progeny[6], 1),
                  s);
              scheduler_splittask(
                  scheduler_addtask(s, task_type_pair, t->subtype, 12, 0,
                                    ci->progeny[7], cj->progeny[6], 1),
                  s);
              break;
          } /* switch(sid) */
        }

        /* Otherwise, break it up if it is too large? */
      } else if (scheduler_doforcesplit && ci->split && cj->split &&
                 (ci->count > space_maxsize / cj->count)) {

        // message( "force splitting pair with %i and %i parts." , ci->count ,
        // cj->count );

        /* Replace the current task. */
        t->type = task_type_none;

        for (int j = 0; j < 8; j++)
          if (ci->progeny[j] != NULL)
            for (int k = 0; k < 8; k++)
              if (cj->progeny[k] != NULL) {
                struct task *tl =
                    scheduler_addtask(s, task_type_pair, t->subtype, 0, 0,
                                      ci->progeny[j], cj->progeny[k], 0);
                scheduler_splittask(tl, s);
                tl->flags = space_getsid(s->space, &t->ci, &t->cj, shift);
              }

        /* Otherwise, if not spilt, stitch-up the sorting. */
      } else {

        /* Create the sort for ci. */
        lock_lock(&ci->lock);
        if (ci->sorts == NULL)
          ci->sorts = scheduler_addtask(s, task_type_sort, task_subtype_none,
                                        1 << sid, 0, ci, NULL, 0);
        else
          ci->sorts->flags |= (1 << sid);
        lock_unlock_blind(&ci->lock);
        scheduler_addunlock(s, ci->sorts, t);

        /* Create the sort for cj. */
        lock_lock(&cj->lock);
        if (cj->sorts == NULL)
          cj->sorts = scheduler_addtask(s, task_type_sort, task_subtype_none,
                                        1 << sid, 0, cj, NULL, 0);
        else
          cj->sorts->flags |= (1 << sid);
        lock_unlock_blind(&cj->lock);
        scheduler_addunlock(s, cj->sorts, t);
      }

    } /* pair interaction? */

    /* Long-range gravity interaction ? */
    else if (t->type == task_type_grav_mm) {

      /* Get a handle on the cells involved. */
      struct cell *ci = t->ci;

      /* Safety thing */
      if (ci->gcount == 0) t->type = task_type_none;

    } /* gravity interaction? */
  }   /* iterate over the current task. */
}

/**
 * @brief Mapper function to split tasks that may be too large.
 *
 * @param map_data the tasks to process
 * @param num_elements the number of tasks.
 * @param extra_data The #scheduler we are working in.
 */

void scheduler_splittasks_mapper(void *map_data, int num_elements,
                                 void *extra_data) {

  /* Extract the parameters. */
  struct scheduler *s = (struct scheduler *)extra_data;
  struct task *tasks = (struct task *)map_data;

  for (int ind = 0; ind < num_elements; ind++) {
    struct task *t = &tasks[ind];
    scheduler_splittask(t, s);
  }
}

void scheduler_splittasks(struct scheduler *s) {

  /* Call the mapper on each current task. */
  threadpool_map(s->threadpool, scheduler_splittasks_mapper, s->tasks,
                 s->nr_tasks, sizeof(struct task), 1000, s);
}

/**
 * @brief Add a #task to the #scheduler.
 *
 * @param s The #scheduler we are working in.
 * @param type The type of the task.
 * @param subtype The sub-type of the task.
 * @param flags The flags of the task.
 * @param wait
 * @param ci The first cell to interact.
 * @param cj The second cell to interact.
 * @param tight
 */

struct task *scheduler_addtask(struct scheduler *s, enum task_types type,
                               enum task_subtypes subtype, int flags, int wait,
                               struct cell *ci, struct cell *cj, int tight) {

  /* Get the next free task. */
  const int ind = atomic_inc(&s->tasks_next);

  /* Overflow? */
  if (ind >= s->size) error("Task list overflow.");

  /* Get a pointer to the new task. */
  struct task *t = &s->tasks[ind];

  /* Copy the data. */
  t->type = type;
  t->subtype = subtype;
  t->flags = flags;
  t->wait = wait;
  t->ci = ci;
  t->cj = cj;
  t->skip = 0;
  t->tight = tight;
  t->implicit = 0;
  t->weight = 0;
  t->rank = 0;
  t->tic = 0;
  t->toc = 0;
  t->nr_unlock_tasks = 0;
  t->rid = -1;

  /* Add an index for it. */
  // lock_lock( &s->lock );
  s->tasks_ind[atomic_inc(&s->nr_tasks)] = ind;
  // lock_unlock_blind( &s->lock );

  /* Return a pointer to the new task. */
  return t;
}

/**
 * @brief Set the unlock pointers in each task.
 *
 * @param s The #scheduler.
 */

void scheduler_set_unlocks(struct scheduler *s) {

  /* Store the counts for each task. */
  int *counts;
  if ((counts = (int *)malloc(sizeof(int) * s->nr_tasks)) == NULL)
    error("Failed to allocate temporary counts array.");
  bzero(counts, sizeof(int) * s->nr_tasks);
  for (int k = 0; k < s->nr_unlocks; k++) counts[s->unlock_ind[k]] += 1;

  /* Compute the offset for each unlock block. */
  int *offsets;
  if ((offsets = (int *)malloc(sizeof(int) * (s->nr_tasks + 1))) == NULL)
    error("Failed to allocate temporary offsets array.");
  offsets[0] = 0;
  for (int k = 0; k < s->nr_tasks; k++) offsets[k + 1] = offsets[k] + counts[k];

  /* Create and fill a temporary array with the sorted unlocks. */
  struct task **unlocks;
  if ((unlocks = (struct task **)malloc(sizeof(struct task *) *
                                        s->size_unlocks)) == NULL)
    error("Failed to allocate temporary unlocks array.");
  for (int k = 0; k < s->nr_unlocks; k++) {
    const int ind = s->unlock_ind[k];
    unlocks[offsets[ind]] = s->unlocks[k];
    offsets[ind] += 1;
  }

  /* Swap the unlocks. */
  free(s->unlocks);
  s->unlocks = unlocks;

  /* Re-set the offsets. */
  offsets[0] = 0;
  for (int k = 1; k < s->nr_tasks; k++)
    offsets[k] = offsets[k - 1] + counts[k - 1];

  /* Set the unlocks in the tasks. */
  for (int k = 0; k < s->nr_tasks; k++) {
    struct task *t = &s->tasks[k];
    t->nr_unlock_tasks = counts[k];
    t->unlock_tasks = &s->unlocks[offsets[k]];
  }

#ifdef SWIFT_DEBUG_CHECKS
  /* Verify that there are no duplicate unlocks. */
  for (int k = 0; k < s->nr_tasks; k++) {
    struct task *t = &s->tasks[k];
    for (int i = 0; i < t->nr_unlock_tasks; i++) {
      for (int j = i + 1; j < t->nr_unlock_tasks; j++) {
        if (t->unlock_tasks[i] == t->unlock_tasks[j])
          error("duplicate unlock!");
      }
    }
  }
#endif

  /* Clean up. */
  free(counts);
  free(offsets);
}

/**
 * @brief Sort the tasks in topological order over all queues.
 *
 * @param s The #scheduler.
 */

void scheduler_ranktasks(struct scheduler *s) {

  struct task *tasks = s->tasks;
  int *tid = s->tasks_ind;
  const int nr_tasks = s->nr_tasks;

  /* Run through the tasks and get all the waits right. */
  for (int i = 0; i < nr_tasks; i++) {
    struct task *t = &tasks[i];

    // Increment the waits of the dependances
    for (int k = 0; k < t->nr_unlock_tasks; k++) {
      t->unlock_tasks[k]->wait++;
    }
  }

  /* Load the tids of tasks with no waits. */
  int left = 0;
  for (int k = 0; k < nr_tasks; k++)
    if (tasks[k].wait == 0) {
      tid[left] = k;
      left += 1;
    }

  /* Main loop. */
  for (int j = 0, rank = 0; left < nr_tasks; rank++) {

    /* Did we get anything? */
    if (j == left) error("Unsatisfiable task dependencies detected.");
    const int left_old = left;

    /* Unlock the next layer of tasks. */
    for (; j < left_old; j++) {
      struct task *t = &tasks[tid[j]];
      t->rank = rank;
      /* message( "task %i of type %s has rank %i." , i ,
          (t->type == task_type_self) ? "self" : (t->type == task_type_pair) ?
         "pair" : "sort" , rank ); */
      for (int k = 0; k < t->nr_unlock_tasks; k++) {
        struct task *u = t->unlock_tasks[k];
        if (--u->wait == 0) {
          tid[left] = u - tasks;
          left += 1;
        }
      }
    }

    /* Move back to the old left (like Sanders). */
    j = left_old;
  }

#ifdef SWIFT_DEBUG_CHECKS
  /* Verify that the tasks were ranked correctly. */
  for (int k = 1; k < s->nr_tasks; k++)
    if (tasks[tid[k - 1]].rank > tasks[tid[k - 1]].rank)
      error("Task ranking failed.");
#endif
}

/**
 * @brief (Re)allocate the task arrays.
 *
 * @param s The #scheduler.
 * @param size The maximum number of tasks in the #scheduler.
 */

void scheduler_reset(struct scheduler *s, int size) {

  /* Do we need to re-allocate? */
  if (size > s->size) {

    /* Free existing task lists if necessary. */
    if (s->tasks != NULL) free(s->tasks);
    if (s->tasks_ind != NULL) free(s->tasks_ind);

    /* Allocate the new lists. */
    if (posix_memalign((void *)&s->tasks, task_align,
                       size * sizeof(struct task)) != 0)
      error("Failed to allocate task array.");

    if ((s->tasks_ind = (int *)malloc(sizeof(int) * size)) == NULL)
      error("Failed to allocate task lists.");
  }

  /* Reset the counters. */
  s->size = size;
  s->nr_tasks = 0;
  s->tasks_next = 0;
  s->waiting = 0;
  s->mask = 0;
  s->submask = 0;
  s->nr_unlocks = 0;
  s->completed_unlock_writes = 0;

  /* Set the task pointers in the queues. */
  for (int k = 0; k < s->nr_queues; k++) s->queues[k].tasks = s->tasks;
}

/**
 * @brief Compute the task weights
 *
 * @param s The #scheduler.
 * @param verbose Are we talkative ?
 */
void scheduler_reweight(struct scheduler *s, int verbose) {

  const ticks tic = getticks();
  const int nr_tasks = s->nr_tasks;
  int *tid = s->tasks_ind;
  struct task *tasks = s->tasks;
  const int nodeID = s->nodeID;
  const float sid_scale[13] = {0.1897, 0.4025, 0.1897, 0.4025, 0.5788,
                               0.4025, 0.1897, 0.4025, 0.1897, 0.4025,
                               0.5788, 0.4025, 0.5788};
  const float wscale = 0.001;

  /* Run through the tasks backwards and set their weights. */
  for (int k = nr_tasks - 1; k >= 0; k--) {
    struct task *t = &tasks[tid[k]];
    t->weight = 0;
    for (int j = 0; j < t->nr_unlock_tasks; j++)
      if (t->unlock_tasks[j]->weight > t->weight)
        t->weight = t->unlock_tasks[j]->weight;
    if (!t->implicit && t->tic > 0)
      t->weight += wscale * (t->toc - t->tic);
    else
      switch (t->type) {
        case task_type_sort:
          t->weight += wscale * intrinsics_popcount(t->flags) * t->ci->count *
                       (sizeof(int) * 8 - intrinsics_clz(t->ci->count));
          break;
        case task_type_self:
          t->weight += 1 * wscale * t->ci->count * t->ci->count;
          break;
        case task_type_pair:
          if (t->ci->nodeID != nodeID || t->cj->nodeID != nodeID)
            t->weight +=
                3 * wscale * t->ci->count * t->cj->count * sid_scale[t->flags];
          else
            t->weight +=
                2 * wscale * t->ci->count * t->cj->count * sid_scale[t->flags];
          break;
        case task_type_sub_pair:
          if (t->ci->nodeID != nodeID || t->cj->nodeID != nodeID) {
            if (t->flags < 0)
              t->weight += 3 * wscale * t->ci->count * t->cj->count;
            else
              t->weight += 3 * wscale * t->ci->count * t->cj->count *
                           sid_scale[t->flags];
          } else {
            if (t->flags < 0)
              t->weight += 2 * wscale * t->ci->count * t->cj->count;
            else
              t->weight += 2 * wscale * t->ci->count * t->cj->count *
                           sid_scale[t->flags];
          }
          break;
        case task_type_sub_self:
          t->weight += 1 * wscale * t->ci->count * t->ci->count;
          break;
        case task_type_ghost:
          if (t->ci == t->ci->super) t->weight += wscale * t->ci->count;
          break;
        case task_type_kick:
          t->weight += wscale * t->ci->count;
          break;
        case task_type_init:
          t->weight += wscale * t->ci->count;
          break;
        default:
          break;
      }
  }

  if (verbose)
    message("took %.3f %s.", clocks_from_ticks(getticks() - tic),
            clocks_getunit());

  /* int min = tasks[0].weight, max = tasks[0].weight;
  for ( int k = 1 ; k < nr_tasks ; k++ )
      if ( tasks[k].weight < min )
          min = tasks[k].weight;
      else if ( tasks[k].weight > max )
          max = tasks[k].weight;
  message( "task weights are in [ %i , %i ]." , min , max ); */
}

/**
 * @brief #threadpool_map function which runs through the task
 *        graph and re-computes the task wait counters.
 */

void scheduler_rewait_mapper(void *map_data, int num_elements,
                             void *extra_data) {

  struct scheduler *s = (struct scheduler *)extra_data;
  struct task *tasks = (struct task *)map_data;

  for (int ind = 0; ind < num_elements; ind++) {
    struct task *t = &tasks[ind];

    if (t->skip || !((1 << t->type) & s->mask) ||
        !((1 << t->subtype) & s->submask))
      continue;

    /* Skip sort tasks that have already been performed */
    if (t->type == task_type_sort && t->flags == 0) {
      error("Empty sort task encountered.");
    }

    /* Sets the waits of the dependances */
    for (int k = 0; k < t->nr_unlock_tasks; k++) {
      struct task *u = t->unlock_tasks[k];
      atomic_inc(&u->wait);
    }
  }
}

void scheduler_enqueue_mapper(void *map_data, int num_elements,
                              void *extra_data) {
  struct scheduler *s = (struct scheduler *)extra_data;
  const int *tid = (int *)map_data;
  struct task *tasks = s->tasks;
  for (int ind = 0; ind < num_elements; ind++) {
    struct task *t = &tasks[tid[ind]];
    if (atomic_dec(&t->wait) == 1 && !t->skip && ((1 << t->type) & s->mask) &&
        ((1 << t->subtype) & s->submask)) {
      scheduler_enqueue(s, t);
    }
  }
  pthread_cond_broadcast(&s->sleep_cond);
}

/**
 * @brief Start the scheduler, i.e. fill the queues with ready tasks.
 *
 * @param s The #scheduler.
 * @param mask The task types to enqueue.
 * @param submask The sub-task types to enqueue.
 */

void scheduler_start(struct scheduler *s, unsigned int mask,
                     unsigned int submask) {

  // ticks tic = getticks();

  /* Store the masks */
  s->mask = mask;
  s->submask = submask | (1 << task_subtype_none);

  /* Clear all the waits and rids. */
  for (int k = 0; k < s->nr_tasks; k++) {
    s->tasks[k].wait = 1;
    s->tasks[k].rid = -1;
  }

  /* Re-wait the tasks. */
  threadpool_map(s->threadpool, scheduler_rewait_mapper, s->tasks, s->nr_tasks,
                 sizeof(struct task), 1000, s);

  /* Loop over the tasks and enqueue whoever is ready. */
  threadpool_map(s->threadpool, scheduler_enqueue_mapper, s->tasks_ind,
                 s->nr_tasks, sizeof(int), 1000, s);

  /* To be safe, fire of one last sleep_cond in a safe way. */
  pthread_mutex_lock(&s->sleep_mutex);
  pthread_cond_broadcast(&s->sleep_cond);
  pthread_mutex_unlock(&s->sleep_mutex);

  /* message("enqueueing tasks took %.3f %s." ,
          clocks_from_ticks( getticks() - tic ), clocks_getunit()); */
}

/**
 * @brief Put a task on one of the queues.
 *
 * @param s The #scheduler.
 * @param t The #task.
 */

void scheduler_enqueue(struct scheduler *s, struct task *t) {

  /* The target queue for this task. */
  int qid = -1;

  /* Fail if this task has already been enqueued before. */
  if (t->rid >= 0) error("Task has already been enqueued.");

  /* Ignore skipped tasks and tasks not in the masks. */
  if (t->skip || (1 << t->type) & ~(s->mask) ||
      (1 << t->subtype) & ~(s->submask)) {
    return;
  }

  /* If this is an implicit task, just pretend it's done. */
  if (t->implicit) {
    for (int j = 0; j < t->nr_unlock_tasks; j++) {
      struct task *t2 = t->unlock_tasks[j];
      if (atomic_dec(&t2->wait) == 1) scheduler_enqueue(s, t2);
    }
  }

  /* Otherwise, look for a suitable queue. */
  else {
#ifdef WITH_MPI
    int err;
#endif

    /* Find the previous owner for each task type, and do
       any pre-processing needed. */
    switch (t->type) {
      case task_type_self:
      case task_type_sub_self:
      case task_type_sort:
      case task_type_ghost:
      case task_type_kick:
      case task_type_init:
        qid = t->ci->super->owner;
        break;
      case task_type_pair:
      case task_type_sub_pair:
        if (t->subtype == task_subtype_grav) {

          qid = t->ci->gsuper->owner;
          if (qid < 0 ||
              s->queues[qid].count > s->queues[t->cj->gsuper->owner].count)
            qid = t->cj->gsuper->owner;

        } else {

          qid = t->ci->super->owner;
          if (qid < 0 ||
              s->queues[qid].count > s->queues[t->cj->super->owner].count)
            qid = t->cj->super->owner;
        }
        break;
      case task_type_recv:
#ifdef WITH_MPI
        if (t->subtype == task_subtype_tend) {
          t->buff = malloc(sizeof(int) * t->ci->pcell_size);
          err = MPI_Irecv(t->buff, t->ci->pcell_size, MPI_INT, t->ci->nodeID,
                          t->flags, MPI_COMM_WORLD, &t->req);
        } else {
          err = MPI_Irecv(t->ci->parts, t->ci->count, part_mpi_type,
                          t->ci->nodeID, t->flags, MPI_COMM_WORLD, &t->req);
        }
        if (err != MPI_SUCCESS) {
          mpi_error(err, "Failed to emit irecv for particle data.");
        }
        // message( "receiving %i parts with tag=%i from %i to %i." ,
        //     t->ci->count , t->flags , t->ci->nodeID , s->nodeID );
        // fflush(stdout);
        qid = 1 % s->nr_queues;
#else
        error("SWIFT was not compiled with MPI support.");
#endif
        break;
      case task_type_send:
#ifdef WITH_MPI
        if (t->subtype == task_subtype_tend) {
          t->buff = malloc(sizeof(int) * t->ci->pcell_size);
          cell_pack_ti_ends(t->ci, t->buff);
          err = MPI_Isend(t->buff, t->ci->pcell_size, MPI_INT, t->cj->nodeID,
                          t->flags, MPI_COMM_WORLD, &t->req);
        } else {
          err = MPI_Isend(t->ci->parts, t->ci->count, part_mpi_type,
                          t->cj->nodeID, t->flags, MPI_COMM_WORLD, &t->req);
        }
        if (err != MPI_SUCCESS) {
          mpi_error(err, "Failed to emit isend for particle data.");
        }
        // message( "sending %i parts with tag=%i from %i to %i." ,
        //     t->ci->count , t->flags , s->nodeID , t->cj->nodeID );
        // fflush(stdout);
        qid = 0;
#else
        error("SWIFT was not compiled with MPI support.");
#endif
        break;
      default:
        qid = -1;
    }

    if (qid >= s->nr_queues) error("Bad computed qid.");

    /* If no previous owner, pick a random queue. */
    if (qid < 0) qid = rand() % s->nr_queues;

    /* Increase the waiting counter. */
    atomic_inc(&s->waiting);

    /* Insert the task into that queue. */
    queue_insert(&s->queues[qid], t);
  }
}

/**
 * @brief Take care of a tasks dependencies.
 *
 * @param s The #scheduler.
 * @param t The finished #task.
 *
 * @return A pointer to the next task, if a suitable one has
 *         been identified.
 */

struct task *scheduler_done(struct scheduler *s, struct task *t) {

  /* Release whatever locks this task held. */
  if (!t->implicit) task_unlock(t);

  /* Loop through the dependencies and add them to a queue if
     they are ready. */
  for (int k = 0; k < t->nr_unlock_tasks; k++) {
    struct task *t2 = t->unlock_tasks[k];

    const int res = atomic_dec(&t2->wait);
    if (res < 1) {
      error("Negative wait!");
    } else if (res == 1) {
      scheduler_enqueue(s, t2);
    }
  }

  /* Task definitely done, signal any sleeping runners. */
  if (!t->implicit) {
    t->toc = getticks();
    pthread_mutex_lock(&s->sleep_mutex);
    atomic_dec(&s->waiting);
    pthread_cond_broadcast(&s->sleep_cond);
    pthread_mutex_unlock(&s->sleep_mutex);
  }

  /* Return the next best task. Note that we currently do not
     implement anything that does this, as getting it to respect
     priorities is too tricky and currently unnecessary. */
  return NULL;
}

/**
 * @brief Resolve a single dependency by hand.
 *
 * @param s The #scheduler.
 * @param t The dependent #task.
 *
 * @return A pointer to the next task, if a suitable one has
 *         been identified.
 */

struct task *scheduler_unlock(struct scheduler *s, struct task *t) {

  /* Loop through the dependencies and add them to a queue if
     they are ready. */
  for (int k = 0; k < t->nr_unlock_tasks; k++) {
    struct task *t2 = t->unlock_tasks[k];
    const int res = atomic_dec(&t2->wait);
    if (res < 1) {
      error("Negative wait!");
    } else if (res == 1) {
      scheduler_enqueue(s, t2);
    }
  }

  /* Task definitely done. */
  if (!t->implicit) {
    t->toc = getticks();
    pthread_mutex_lock(&s->sleep_mutex);
    atomic_dec(&s->waiting);
    pthread_cond_broadcast(&s->sleep_cond);
    pthread_mutex_unlock(&s->sleep_mutex);
  }

  /* Return the next best task. Note that we currently do not
     implement anything that does this, as getting it to respect
     priorities is too tricky and currently unnecessary. */
  return NULL;
}

/**
 * @brief Get a task, preferably from the given queue.
 *
 * @param s The #scheduler.
 * @param qid The ID of the preferred #queue.
 * @param prev the previous task that was run.
 *
 * @return A pointer to a #task or @c NULL if there are no available tasks.
 */

struct task *scheduler_gettask(struct scheduler *s, int qid,
                               const struct task *prev) {

  struct task *res = NULL;
  const int nr_queues = s->nr_queues;
  unsigned int seed = qid;

  /* Check qid. */
  if (qid >= nr_queues || qid < 0) error("Bad queue ID.");

  /* Loop as long as there are tasks... */
  while (s->waiting > 0 && res == NULL) {

    /* Try more than once before sleeping. */
    for (int tries = 0; res == NULL && s->waiting && tries < scheduler_maxtries;
         tries++) {

      /* Try to get a task from the suggested queue. */
      if (s->queues[qid].count > 0 || s->queues[qid].count_incoming > 0) {
        TIMER_TIC
        res = queue_gettask(&s->queues[qid], prev, 0);
        TIMER_TOC(timer_qget);
        if (res != NULL) break;
      }

      /* If unsuccessful, try stealing from the other queues. */
      if (s->flags & scheduler_flag_steal) {
        int count = 0, qids[nr_queues];
        for (int k = 0; k < nr_queues; k++)
          if (s->queues[k].count > 0 || s->queues[k].count_incoming > 0) {
            qids[count++] = k;
          }
        for (int k = 0; k < scheduler_maxsteal && count > 0; k++) {
          const int ind = rand_r(&seed) % count;
          TIMER_TIC
          res = queue_gettask(&s->queues[qids[ind]], prev, 0);
          TIMER_TOC(timer_qsteal);
          if (res != NULL)
            break;
          else
            qids[ind] = qids[--count];
        }
        if (res != NULL) break;
      }
    }

/* If we failed, take a short nap. */
#ifdef WITH_MPI
    if (res == NULL && qid > 1) {
#else
    if (res == NULL) {
#endif
      pthread_mutex_lock(&s->sleep_mutex);
      res = queue_gettask(&s->queues[qid], prev, 1);
      if (res == NULL && s->waiting > 0) {
        pthread_cond_wait(&s->sleep_cond, &s->sleep_mutex);
      }
      pthread_mutex_unlock(&s->sleep_mutex);
    }
  }

  /* Start the timer on this task, if we got one. */
  if (res != NULL) {
    res->tic = getticks();
    res->rid = qid;
  }

  /* No milk today. */
  return res;
}

/**
 * @brief Initialize the #scheduler.
 *
 * @param s The #scheduler.
 * @param space The #space we are working with
 * @param nr_tasks The number of tasks to allocate initially.
 * @param nr_queues The number of queues in this scheduler.
 * @param flags The #scheduler flags.
 * @param nodeID The MPI rank
 * @param tp Parallel processing threadpool.
 */

void scheduler_init(struct scheduler *s, struct space *space, int nr_tasks,
                    int nr_queues, unsigned int flags, int nodeID,
                    struct threadpool *tp) {

  /* Init the lock. */
  lock_init(&s->lock);

  /* Allocate the queues. */
  if ((s->queues = (struct queue *)malloc(sizeof(struct queue) * nr_queues)) ==
      NULL)
    error("Failed to allocate queues.");

  /* Initialize each queue. */
  for (int k = 0; k < nr_queues; k++) queue_init(&s->queues[k], NULL);

  /* Init the sleep mutex and cond. */
  if (pthread_cond_init(&s->sleep_cond, NULL) != 0 ||
      pthread_mutex_init(&s->sleep_mutex, NULL) != 0)
    error("Failed to initialize sleep barrier.");

  /* Init the unlocks. */
  if ((s->unlocks = (struct task **)malloc(
           sizeof(struct task *) * scheduler_init_nr_unlocks)) == NULL ||
      (s->unlock_ind =
           (int *)malloc(sizeof(int) * scheduler_init_nr_unlocks)) == NULL)
    error("Failed to allocate unlocks.");
  s->nr_unlocks = 0;
  s->size_unlocks = scheduler_init_nr_unlocks;

  /* Set the scheduler variables. */
  s->nr_queues = nr_queues;
  s->flags = flags;
  s->space = space;
  s->nodeID = nodeID;
  s->threadpool = tp;

  /* Init the tasks array. */
  s->size = 0;
  s->tasks = NULL;
  s->tasks_ind = NULL;
  scheduler_reset(s, nr_tasks);
}

/**
 * @brief Prints the list of tasks to a file
 *
 * @param s The #scheduler
 * @param fileName Name of the file to write to
 */
void scheduler_print_tasks(const struct scheduler *s, const char *fileName) {

  const int nr_tasks = s->nr_tasks, *tid = s->tasks_ind;
  struct task *t, *tasks = s->tasks;

  FILE *file = fopen(fileName, "w");

  fprintf(file, "# Rank  Name  Subname  unlocks  waits\n");

  for (int k = nr_tasks - 1; k >= 0; k--) {
    t = &tasks[tid[k]];
    if (!((1 << t->type)) || t->skip) continue;
    fprintf(file, "%d %s %s %d %d\n", k, taskID_names[t->type],
            subtaskID_names[t->subtype], t->nr_unlock_tasks, t->wait);
  }

  fclose(file);
}

/**
 * @brief Frees up the memory allocated for this #scheduler
 */
void scheduler_clean(struct scheduler *s) {

  free(s->tasks);
  free(s->tasks_ind);
  free(s->unlocks);
  free(s->unlock_ind);
  for (int i = 0; i < s->nr_queues; ++i) queue_clean(&s->queues[i]);
  free(s->queues);
}
