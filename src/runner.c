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

/* Config parameters. */
#include "../config.h"

/* Some standard headers. */
#include <float.h>
#include <limits.h>
#include <stdlib.h>

/* MPI headers. */
#ifdef WITH_MPI
#include <mpi.h>
#endif

/* This object's header. */
#include "runner.h"

/* Local headers. */
#include "approx_math.h"
#include "atomic.h"
#include "cell.h"
#include "const.h"
#include "cooling.h"
#include "debug.h"
#include "drift.h"
#include "engine.h"
#include "error.h"
#include "gravity.h"
#include "hydro.h"
#include "hydro_properties.h"
#include "kick.h"
#include "minmax.h"
#include "scheduler.h"
#include "sourceterms.h"
#include "space.h"
#include "task.h"
#include "timers.h"
#include "timestep.h"

/**
 * @brief  Entry in a list of sorted indices.
 */
struct entry {

  /*! Distance on the axis */
  float d;

  /*! Particle index */
  int i;
};

/* Orientation of the cell pairs */
const double runner_shift[13][3] = {
    {5.773502691896258e-01, 5.773502691896258e-01, 5.773502691896258e-01},
    {7.071067811865475e-01, 7.071067811865475e-01, 0.0},
    {5.773502691896258e-01, 5.773502691896258e-01, -5.773502691896258e-01},
    {7.071067811865475e-01, 0.0, 7.071067811865475e-01},
    {1.0, 0.0, 0.0},
    {7.071067811865475e-01, 0.0, -7.071067811865475e-01},
    {5.773502691896258e-01, -5.773502691896258e-01, 5.773502691896258e-01},
    {7.071067811865475e-01, -7.071067811865475e-01, 0.0},
    {5.773502691896258e-01, -5.773502691896258e-01, -5.773502691896258e-01},
    {0.0, 7.071067811865475e-01, 7.071067811865475e-01},
    {0.0, 1.0, 0.0},
    {0.0, 7.071067811865475e-01, -7.071067811865475e-01},
    {0.0, 0.0, 1.0},
};

/* Does the axis need flipping ? */
const char runner_flip[27] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0,
                              0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

/* Import the density loop functions. */
#define FUNCTION density
#include "runner_doiact.h"

/* Import the gradient loop functions (if required). */
#ifdef EXTRA_HYDRO_LOOP
#undef FUNCTION
#define FUNCTION gradient
#include "runner_doiact.h"
#endif

/* Import the force loop functions. */
#undef FUNCTION
#define FUNCTION force
#include "runner_doiact.h"

/* Import the gravity loop functions. */
#include "runner_doiact_fft.h"
#include "runner_doiact_grav.h"

/**
 * @brief Perform source terms
 *
 * @param r runner task
 * @param c cell
 * @param timer 1 if the time is to be recorded.
 */
void runner_do_sourceterms(struct runner *r, struct cell *c, int timer) {
  const int count = c->count;
  const double cell_min[3] = {c->loc[0], c->loc[1], c->loc[2]};
  const double cell_width[3] = {c->width[0], c->width[1], c->width[2]};
  struct sourceterms *sourceterms = r->e->sourceterms;
  const int dimen = 3;

  TIMER_TIC;

  /* Recurse? */
  if (c->split) {
    for (int k = 0; k < 8; k++)
      if (c->progeny[k] != NULL) runner_do_sourceterms(r, c->progeny[k], 0);
    return;
  }

  if (count > 0) {

    /* do sourceterms in this cell? */
    const int incell =
        sourceterms_test_cell(cell_min, cell_width, sourceterms, dimen);
    if (incell == 1) {
      sourceterms_apply(r, sourceterms, c);
    }
  }

  if (timer) TIMER_TOC(timer_dosource);
}

/**
 * @brief Calculate gravity acceleration from external potential
 *
 * @param r runner task
 * @param c cell
 * @param timer 1 if the time is to be recorded.
 */
void runner_do_grav_external(struct runner *r, struct cell *c, int timer) {

  struct gpart *restrict gparts = c->gparts;
  const int gcount = c->gcount;
  const int ti_current = r->e->ti_current;
  const struct external_potential *potential = r->e->external_potential;
  const struct phys_const *constants = r->e->physical_constants;
  const double time = r->e->time;

  TIMER_TIC;

  /* Anything to do here? */
  if (c->ti_end_min > ti_current) return;

  /* Recurse? */
  if (c->split) {
    for (int k = 0; k < 8; k++)
      if (c->progeny[k] != NULL) runner_do_grav_external(r, c->progeny[k], 0);
    return;
  }

#ifdef TASK_VERBOSE
  OUT;
#endif

  /* Loop over the gparts in this cell. */
  for (int i = 0; i < gcount; i++) {

    /* Get a direct pointer on the part. */
    struct gpart *restrict g = &gparts[i];

    /* Is this part within the time step? */
    if (g->ti_end <= ti_current) {

      external_gravity_acceleration(time, potential, constants, g);
    }
  }

  if (timer) TIMER_TOC(timer_dograv_external);
}

/**
 * @brief Calculate change in thermal state of particles induced
 * by radiative cooling and heating.
 *
 * @param r runner task
 * @param c cell
 * @param timer 1 if the time is to be recorded.
 */
void runner_do_cooling(struct runner *r, struct cell *c, int timer) {

  struct part *restrict parts = c->parts;
  struct xpart *restrict xparts = c->xparts;
  const int count = c->count;
  const int ti_current = r->e->ti_current;
  const struct cooling_function_data *cooling_func = r->e->cooling_func;
  const struct phys_const *constants = r->e->physical_constants;
  const struct UnitSystem *us = r->e->internalUnits;
  const double timeBase = r->e->timeBase;

  TIMER_TIC;

  /* Recurse? */
  if (c->split) {
    for (int k = 0; k < 8; k++)
      if (c->progeny[k] != NULL) runner_do_cooling(r, c->progeny[k], 0);
    return;
  }

#ifdef TASK_VERBOSE
  OUT;
#endif

  /* Loop over the parts in this cell. */
  for (int i = 0; i < count; i++) {

    /* Get a direct pointer on the part. */
    struct part *restrict p = &parts[i];
    struct xpart *restrict xp = &xparts[i];

    /* Kick has already updated ti_end, so need to check ti_begin */
    if (p->ti_begin == ti_current) {

      const double dt = (p->ti_end - p->ti_begin) * timeBase;

      cooling_cool_part(constants, us, cooling_func, p, xp, dt);
    }
  }

  if (timer) TIMER_TOC(timer_do_cooling);
}

/**
 * @brief Sort the entries in ascending order using QuickSort.
 *
 * @param sort The entries
 * @param N The number of entries.
 */
void runner_do_sort_ascending(struct entry *sort, int N) {

  struct {
    short int lo, hi;
  } qstack[10];
  int qpos, i, j, lo, hi, imin;
  struct entry temp;
  float pivot;

  /* Sort parts in cell_i in decreasing order with quicksort */
  qstack[0].lo = 0;
  qstack[0].hi = N - 1;
  qpos = 0;
  while (qpos >= 0) {
    lo = qstack[qpos].lo;
    hi = qstack[qpos].hi;
    qpos -= 1;
    if (hi - lo < 15) {
      for (i = lo; i < hi; i++) {
        imin = i;
        for (j = i + 1; j <= hi; j++)
          if (sort[j].d < sort[imin].d) imin = j;
        if (imin != i) {
          temp = sort[imin];
          sort[imin] = sort[i];
          sort[i] = temp;
        }
      }
    } else {
      pivot = sort[(lo + hi) / 2].d;
      i = lo;
      j = hi;
      while (i <= j) {
        while (sort[i].d < pivot) i++;
        while (sort[j].d > pivot) j--;
        if (i <= j) {
          if (i < j) {
            temp = sort[i];
            sort[i] = sort[j];
            sort[j] = temp;
          }
          i += 1;
          j -= 1;
        }
      }
      if (j > (lo + hi) / 2) {
        if (lo < j) {
          qpos += 1;
          qstack[qpos].lo = lo;
          qstack[qpos].hi = j;
        }
        if (i < hi) {
          qpos += 1;
          qstack[qpos].lo = i;
          qstack[qpos].hi = hi;
        }
      } else {
        if (i < hi) {
          qpos += 1;
          qstack[qpos].lo = i;
          qstack[qpos].hi = hi;
        }
        if (lo < j) {
          qpos += 1;
          qstack[qpos].lo = lo;
          qstack[qpos].hi = j;
        }
      }
    }
  }
}

/**
 * @brief Sort the particles in the given cell along all cardinal directions.
 *
 * @param r The #runner.
 * @param c The #cell.
 * @param flags Cell flag.
 * @param clock Flag indicating whether to record the timing or not, needed
 *      for recursive calls.
 */
void runner_do_sort(struct runner *r, struct cell *c, int flags, int clock) {

  struct entry *finger;
  struct entry *fingers[8];
  struct part *parts = c->parts;
  struct entry *sort;
  int j, k, count = c->count;
  int i, ind, off[8], inds[8], temp_i, missing;
  float buff[8];
  double px[3];

  TIMER_TIC

  /* Clean-up the flags, i.e. filter out what's already been sorted. */
  flags &= ~c->sorted;
  if (flags == 0) return;

  /* start by allocating the entry arrays. */
  if (c->sort == NULL || c->sortsize < count) {
    if (c->sort != NULL) free(c->sort);
    c->sortsize = count * 1.1;
    if ((c->sort = (struct entry *)malloc(sizeof(struct entry) *
                                          (c->sortsize + 1) * 13)) == NULL)
      error("Failed to allocate sort memory.");
  }
  sort = c->sort;

  /* Does this cell have any progeny? */
  if (c->split) {

    /* Fill in the gaps within the progeny. */
    for (k = 0; k < 8; k++) {
      if (c->progeny[k] == NULL) continue;
      missing = flags & ~c->progeny[k]->sorted;
      if (missing) runner_do_sort(r, c->progeny[k], missing, 0);
    }

    /* Loop over the 13 different sort arrays. */
    for (j = 0; j < 13; j++) {

      /* Has this sort array been flagged? */
      if (!(flags & (1 << j))) continue;

      /* Init the particle index offsets. */
      for (off[0] = 0, k = 1; k < 8; k++)
        if (c->progeny[k - 1] != NULL)
          off[k] = off[k - 1] + c->progeny[k - 1]->count;
        else
          off[k] = off[k - 1];

      /* Init the entries and indices. */
      for (k = 0; k < 8; k++) {
        inds[k] = k;
        if (c->progeny[k] != NULL && c->progeny[k]->count > 0) {
          fingers[k] = &c->progeny[k]->sort[j * (c->progeny[k]->count + 1)];
          buff[k] = fingers[k]->d;
          off[k] = off[k];
        } else
          buff[k] = FLT_MAX;
      }

      /* Sort the buffer. */
      for (i = 0; i < 7; i++)
        for (k = i + 1; k < 8; k++)
          if (buff[inds[k]] < buff[inds[i]]) {
            temp_i = inds[i];
            inds[i] = inds[k];
            inds[k] = temp_i;
          }

      /* For each entry in the new sort list. */
      finger = &sort[j * (count + 1)];
      for (ind = 0; ind < count; ind++) {

        /* Copy the minimum into the new sort array. */
        finger[ind].d = buff[inds[0]];
        finger[ind].i = fingers[inds[0]]->i + off[inds[0]];

        /* Update the buffer. */
        fingers[inds[0]] += 1;
        buff[inds[0]] = fingers[inds[0]]->d;

        /* Find the smallest entry. */
        for (k = 1; k < 8 && buff[inds[k]] < buff[inds[k - 1]]; k++) {
          temp_i = inds[k - 1];
          inds[k - 1] = inds[k];
          inds[k] = temp_i;
        }

      } /* Merge. */

      /* Add a sentinel. */
      sort[j * (count + 1) + count].d = FLT_MAX;
      sort[j * (count + 1) + count].i = 0;

      /* Mark as sorted. */
      c->sorted |= (1 << j);

    } /* loop over sort arrays. */

  } /* progeny? */

  /* Otherwise, just sort. */
  else {

    /* Fill the sort array. */
    for (k = 0; k < count; k++) {
      px[0] = parts[k].x[0];
      px[1] = parts[k].x[1];
      px[2] = parts[k].x[2];
      for (j = 0; j < 13; j++)
        if (flags & (1 << j)) {
          sort[j * (count + 1) + k].i = k;
          sort[j * (count + 1) + k].d = px[0] * runner_shift[j][0] +
                                        px[1] * runner_shift[j][1] +
                                        px[2] * runner_shift[j][2];
        }
    }

    /* Add the sentinel and sort. */
    for (j = 0; j < 13; j++)
      if (flags & (1 << j)) {
        sort[j * (count + 1) + count].d = FLT_MAX;
        sort[j * (count + 1) + count].i = 0;
        runner_do_sort_ascending(&sort[j * (count + 1)], count);
        c->sorted |= (1 << j);
      }
  }

#ifdef SWIFT_DEBUG_CHECKS
  /* Verify the sorting. */
  for (j = 0; j < 13; j++) {
    if (!(flags & (1 << j))) continue;
    finger = &sort[j * (count + 1)];
    for (k = 1; k < count; k++) {
      if (finger[k].d < finger[k - 1].d)
        error("Sorting failed, ascending array.");
      if (finger[k].i >= count) error("Sorting failed, indices borked.");
    }
  }
#endif

  if (clock) TIMER_TOC(timer_dosort);
}

/**
 * @brief Initialize the particles before the density calculation
 *
 * @param r The runner thread.
 * @param c The cell.
 * @param timer 1 if the time is to be recorded.
 */
void runner_do_init(struct runner *r, struct cell *c, int timer) {

  struct part *restrict parts = c->parts;
  struct gpart *restrict gparts = c->gparts;
  const int count = c->count;
  const int gcount = c->gcount;
  const int ti_current = r->e->ti_current;

  TIMER_TIC;

  /* Anything to do here? */
  if (c->ti_end_min > ti_current) return;

  /* Recurse? */
  if (c->split) {
    for (int k = 0; k < 8; k++)
      if (c->progeny[k] != NULL) runner_do_init(r, c->progeny[k], 0);
    return;
  } else {

    /* Loop over the parts in this cell. */
    for (int i = 0; i < count; i++) {

      /* Get a direct pointer on the part. */
      struct part *restrict p = &parts[i];

      if (p->ti_end <= ti_current) {

        /* Get ready for a density calculation */
        hydro_init_part(p);
      }
    }

    /* Loop over the gparts in this cell. */
    for (int i = 0; i < gcount; i++) {

      /* Get a direct pointer on the part. */
      struct gpart *restrict gp = &gparts[i];

      if (gp->ti_end <= ti_current) {

        /* Get ready for a density calculation */
        gravity_init_gpart(gp);
      }
    }
  }

  if (timer) TIMER_TOC(timer_init);
}

/**
 * @brief Intermediate task after the gradient loop that does final operations
 * on the gradient quantities and optionally slope limits the gradients
 *
 * @param r The runner thread.
 * @param c The cell.
 */
void runner_do_extra_ghost(struct runner *r, struct cell *c) {

#ifdef EXTRA_HYDRO_LOOP

  struct part *restrict parts = c->parts;
  const int count = c->count;
  const int ti_current = r->e->ti_current;

  /* Anything to do here? */
  if (c->ti_end_min > ti_current) return;

  /* Recurse? */
  if (c->split) {
    for (int k = 0; k < 8; k++)
      if (c->progeny[k] != NULL) runner_do_extra_ghost(r, c->progeny[k]);
    return;
  } else {

    /* Loop over the parts in this cell. */
    for (int i = 0; i < count; i++) {

      /* Get a direct pointer on the part. */
      struct part *restrict p = &parts[i];

      if (p->ti_end <= ti_current) {

        /* Get ready for a force calculation */
        hydro_end_gradient(p);
      }
    }
  }

#else
  error("SWIFT was not compiled with the extra hydro loop activated.");
#endif
}

/**
 * @brief Intermediate task after the density to check that the smoothing
 * lengths are correct.
 *
 * @param r The runner thread.
 * @param c The cell.
 */
void runner_do_ghost(struct runner *r, struct cell *c) {

  struct part *restrict parts = c->parts;
  struct xpart *restrict xparts = c->xparts;
  int redo, count = c->count;
  const int ti_current = r->e->ti_current;
  const double timeBase = r->e->timeBase;
  const float target_wcount = r->e->hydro_properties->target_neighbours;
  const float max_wcount =
      target_wcount + r->e->hydro_properties->delta_neighbours;
  const float min_wcount =
      target_wcount - r->e->hydro_properties->delta_neighbours;
  const int max_smoothing_iter =
      r->e->hydro_properties->max_smoothing_iterations;

  TIMER_TIC;

  /* Anything to do here? */
  if (c->ti_end_min > ti_current) return;

  /* Recurse? */
  if (c->split) {
    for (int k = 0; k < 8; k++)
      if (c->progeny[k] != NULL) runner_do_ghost(r, c->progeny[k]);
    return;
  }

  /* Init the IDs that have to be updated. */
  int *pid = NULL;
  if ((pid = malloc(sizeof(int) * count)) == NULL)
    error("Can't allocate memory for pid.");
  for (int k = 0; k < count; k++) pid[k] = k;

  /* While there are particles that need to be updated... */
  for (int num_reruns = 0; count > 0 && num_reruns < max_smoothing_iter;
       num_reruns++) {

    /* Reset the redo-count. */
    redo = 0;

    /* Loop over the parts in this cell. */
    for (int i = 0; i < count; i++) {

      /* Get a direct pointer on the part. */
      struct part *restrict p = &parts[pid[i]];
      struct xpart *restrict xp = &xparts[pid[i]];

      /* Is this part within the timestep? */
      if (p->ti_end <= ti_current) {

        /* Finish the density calculation */
        hydro_end_density(p, ti_current);

        float h_corr = 0.f;

        /* If no derivative, double the smoothing length. */
        if (p->density.wcount_dh == 0.0f) h_corr = p->h;

        /* Otherwise, compute the smoothing length update (Newton step). */
        else {
          h_corr = (target_wcount - p->density.wcount) / p->density.wcount_dh;

          /* Truncate to the range [ -p->h/2 , p->h ]. */
          h_corr = (h_corr < p->h) ? h_corr : p->h;
          h_corr = (h_corr > -0.5f * p->h) ? h_corr : -0.5f * p->h;
        }

        /* Did we get the right number density? */
        if (p->density.wcount > max_wcount || p->density.wcount < min_wcount) {

          /* Ok, correct then */
          p->h += h_corr;

          /* Flag for another round of fun */
          pid[redo] = pid[i];
          redo += 1;

          /* Re-initialise everything */
          hydro_init_part(p);

          /* Off we go ! */
          continue;
        }

        /* We now have a particle whose smoothing length has converged */

        /* As of here, particle force variables will be set. */

        /* Compute variables required for the force loop */
        hydro_prepare_force(p, xp, ti_current, timeBase);

        /* The particle force values are now set.  Do _NOT_
           try to read any particle density variables! */

        /* Prepare the particle for the force loop over neighbours */
        hydro_reset_acceleration(p);
      }
    }

    /* We now need to treat the particles whose smoothing length had not
     * converged again */

    /* Re-set the counter for the next loop (potentially). */
    count = redo;
    if (count > 0) {

      /* Climb up the cell hierarchy. */
      for (struct cell *finger = c; finger != NULL; finger = finger->parent) {

        /* Run through this cell's density interactions. */
        for (struct link *l = finger->density; l != NULL; l = l->next) {

          /* Self-interaction? */
          if (l->t->type == task_type_self)
            runner_doself_subset_density(r, finger, parts, pid, count);

          /* Otherwise, pair interaction? */
          else if (l->t->type == task_type_pair) {

            /* Left or right? */
            if (l->t->ci == finger)
              runner_dopair_subset_density(r, finger, parts, pid, count,
                                           l->t->cj);
            else
              runner_dopair_subset_density(r, finger, parts, pid, count,
                                           l->t->ci);

          }

          /* Otherwise, sub-self interaction? */
          else if (l->t->type == task_type_sub_self)
            runner_dosub_subset_density(r, finger, parts, pid, count, NULL, -1,
                                        1);

          /* Otherwise, sub-pair interaction? */
          else if (l->t->type == task_type_sub_pair) {

            /* Left or right? */
            if (l->t->ci == finger)
              runner_dosub_subset_density(r, finger, parts, pid, count,
                                          l->t->cj, -1, 1);
            else
              runner_dosub_subset_density(r, finger, parts, pid, count,
                                          l->t->ci, -1, 1);
          }
        }
      }
    }
  }

  if (count)
    message("Smoothing length failed to converge on %i particles.", count);

  /* Be clean */
  free(pid);

  TIMER_TOC(timer_do_ghost);
}

/**
 * @brief Drift particles and g-particles in a cell forward in time
 *
 * @param c The cell.
 * @param e The engine.
 */
static void runner_do_drift(struct cell *c, struct engine *e) {

  const double timeBase = e->timeBase;
  const int ti_old = c->ti_old;
  const int ti_current = e->ti_current;
  struct part *const parts = c->parts;
  struct xpart *const xparts = c->xparts;
  struct gpart *const gparts = c->gparts;

  /* Do we need to drift ? */
  if (!e->drift_all && !cell_is_drift_needed(c, ti_current)) return;

  /* Check that we are actually going to move forward. */
  if (ti_current == ti_old) return;

  /* Drift from the last time the cell was drifted to the current time */
  const double dt = (ti_current - ti_old) * timeBase;

  float dx_max = 0.f, dx2_max = 0.f, h_max = 0.f;
  double e_kin = 0.0, e_int = 0.0, e_pot = 0.0, e_rad = 0.0;
  double entropy = 0.0, mass = 0.0;
  double mom[3] = {0.0, 0.0, 0.0};
  double ang_mom[3] = {0.0, 0.0, 0.0};

  /* No children? */
  if (!c->split) {

    /* Loop over all the g-particles in the cell */
    const size_t nr_gparts = c->gcount;
    for (size_t k = 0; k < nr_gparts; k++) {

      /* Get a handle on the gpart. */
      struct gpart *const gp = &gparts[k];

      /* Drift... */
      drift_gpart(gp, dt, timeBase, ti_old, ti_current);

      /* Compute (square of) motion since last cell construction */
      const float dx2 = gp->x_diff[0] * gp->x_diff[0] +
                        gp->x_diff[1] * gp->x_diff[1] +
                        gp->x_diff[2] * gp->x_diff[2];
      dx2_max = (dx2_max > dx2) ? dx2_max : dx2;
    }

    /* Loop over all the particles in the cell (more work for these !) */
    const size_t nr_parts = c->count;
    for (size_t k = 0; k < nr_parts; k++) {

      /* Get a handle on the part. */
      struct part *const p = &parts[k];
      struct xpart *const xp = &xparts[k];

      /* Drift... */
      drift_part(p, xp, dt, timeBase, ti_old, ti_current);

      /* Compute (square of) motion since last cell construction */
      const float dx2 = xp->x_diff[0] * xp->x_diff[0] +
                        xp->x_diff[1] * xp->x_diff[1] +
                        xp->x_diff[2] * xp->x_diff[2];
      dx2_max = (dx2_max > dx2) ? dx2_max : dx2;

      /* Maximal smoothing length */
      h_max = (h_max > p->h) ? h_max : p->h;

      /* Now collect quantities for statistics */

      const float half_dt =
          (ti_current - (p->ti_begin + p->ti_end) / 2) * timeBase;
      const double x[3] = {p->x[0], p->x[1], p->x[2]};
      const float v[3] = {xp->v_full[0] + p->a_hydro[0] * half_dt,
                          xp->v_full[1] + p->a_hydro[1] * half_dt,
                          xp->v_full[2] + p->a_hydro[2] * half_dt};

      const float m = hydro_get_mass(p);

      /* Collect mass */
      mass += m;

      /* Collect momentum */
      mom[0] += m * v[0];
      mom[1] += m * v[1];
      mom[2] += m * v[2];

      /* Collect angular momentum */
      ang_mom[0] += m * (x[1] * v[2] - x[2] * v[1]);
      ang_mom[1] += m * (x[2] * v[0] - x[0] * v[2]);
      ang_mom[2] += m * (x[0] * v[1] - x[1] * v[0]);

      /* Collect energies. */
      e_kin += 0.5 * m * (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      e_pot += 0.;
      e_int += m * hydro_get_internal_energy(p, half_dt);
      e_rad += cooling_get_radiated_energy(xp);

      /* Collect entropy */
      entropy += m * hydro_get_entropy(p, half_dt);
    }

    /* Now, get the maximal particle motion from its square */
    dx_max = sqrtf(dx2_max);
  }

  /* Otherwise, aggregate data from children. */
  else {

    /* Loop over the progeny and collect their data. */
    for (int k = 0; k < 8; k++)
      if (c->progeny[k] != NULL) {
        struct cell *cp = c->progeny[k];

        /* Recurse. */
        runner_do_drift(cp, e);

        dx_max = max(dx_max, cp->dx_max);
        h_max = max(h_max, cp->h_max);
        mass += cp->mass;
        e_kin += cp->e_kin;
        e_int += cp->e_int;
        e_pot += cp->e_pot;
        e_rad += cp->e_rad;
        entropy += cp->entropy;
        mom[0] += cp->mom[0];
        mom[1] += cp->mom[1];
        mom[2] += cp->mom[2];
        ang_mom[0] += cp->ang_mom[0];
        ang_mom[1] += cp->ang_mom[1];
        ang_mom[2] += cp->ang_mom[2];
      }
  }

  /* Store the values */
  c->h_max = h_max;
  c->dx_max = dx_max;
  c->mass = mass;
  c->e_kin = e_kin;
  c->e_int = e_int;
  c->e_pot = e_pot;
  c->e_rad = e_rad;
  c->entropy = entropy;
  c->mom[0] = mom[0];
  c->mom[1] = mom[1];
  c->mom[2] = mom[2];
  c->ang_mom[0] = ang_mom[0];
  c->ang_mom[1] = ang_mom[1];
  c->ang_mom[2] = ang_mom[2];

  /* Update the time of the last drift */
  c->ti_old = ti_current;
}

/**
 * @brief Mapper function to drift particles and g-particles forward in time.
 *
 * @param map_data An array of #cell%s.
 * @param num_elements Chunk size.
 * @param extra_data Pointer to an #engine.
 */

void runner_do_drift_mapper(void *map_data, int num_elements,
                            void *extra_data) {

  struct engine *e = (struct engine *)extra_data;
  struct cell *cells = (struct cell *)map_data;

  for (int ind = 0; ind < num_elements; ind++) {
    struct cell *c = &cells[ind];

    /* Only drift local particles. */
    if (c != NULL && c->nodeID == e->nodeID) runner_do_drift(c, e);
  }
}

/**
 * @brief Kick particles in momentum space and collect statistics (fixed
 * time-step case)
 *
 * @param r The runner thread.
 * @param c The cell.
 * @param timer Are we timing this ?
 */
void runner_do_kick_fixdt(struct runner *r, struct cell *c, int timer) {

  const double global_dt = r->e->dt_max;
  const double timeBase = r->e->timeBase;
  const int count = c->count;
  const int gcount = c->gcount;
  struct part *restrict parts = c->parts;
  struct xpart *restrict xparts = c->xparts;
  struct gpart *restrict gparts = c->gparts;
  const double const_G = r->e->physical_constants->const_newton_G;

  int updated = 0, g_updated = 0;
  int ti_end_min = max_nr_timesteps, ti_end_max = 0;

  TIMER_TIC

#ifdef TASK_VERBOSE
  OUT;
#endif

  /* The new time-step */
  const int new_dti = global_dt / timeBase;

  /* No children? */
  if (!c->split) {

    /* Loop over the g-particles and kick everyone. */
    for (int k = 0; k < gcount; k++) {

      /* Get a handle on the part. */
      struct gpart *restrict gp = &gparts[k];

      /* If the g-particle has no counterpart */
      if (gp->id_or_neg_offset > 0) {

        /* First, finish the force calculation */
        gravity_end_force(gp, const_G);

        /* Kick the g-particle forward */
        kick_gpart(gp, new_dti, timeBase);

        /* Number of updated g-particles */
        g_updated++;

        /* Minimal time for next end of time-step */
        ti_end_min = min(gp->ti_end, ti_end_min);
        ti_end_max = max(gp->ti_end, ti_end_max);
      }
    }

    /* Now do the hydro ones... */

    /* Loop over the particles and kick everyone. */
    for (int k = 0; k < count; k++) {

      /* Get a handle on the part. */
      struct part *restrict p = &parts[k];
      struct xpart *restrict xp = &xparts[k];

      /* First, finish the force loop */
      hydro_end_force(p);
      if (p->gpart != NULL) gravity_end_force(p->gpart, const_G);

      /* Kick the particle forward */
      kick_part(p, xp, new_dti, timeBase);

      /* Number of updated particles */
      updated++;
      if (p->gpart != NULL) g_updated++;

      /* Minimal time for next end of time-step */
      ti_end_min = min(p->ti_end, ti_end_min);
      ti_end_max = max(p->ti_end, ti_end_max);
    }
  }

  /* Otherwise, aggregate data from children. */
  else {

    /* Loop over the progeny. */
    for (int k = 0; k < 8; k++)
      if (c->progeny[k] != NULL) {
        struct cell *restrict cp = c->progeny[k];

        /* Recurse */
        runner_do_kick_fixdt(r, cp, 0);

        /* And aggregate */
        updated += cp->updated;
        g_updated += cp->g_updated;
        ti_end_min = min(cp->ti_end_min, ti_end_min);
        ti_end_max = max(cp->ti_end_max, ti_end_max);
      }
  }

  /* Store the values. */
  c->updated = updated;
  c->g_updated = g_updated;
  c->ti_end_min = ti_end_min;
  c->ti_end_max = ti_end_max;

  if (timer) TIMER_TOC(timer_kick);
}

/**
 * @brief Kick particles in momentum space and collect statistics (floating
 * time-step case)
 *
 * @param r The runner thread.
 * @param c The cell.
 * @param timer Are we timing this ?
 */
void runner_do_kick(struct runner *r, struct cell *c, int timer) {

  const struct engine *e = r->e;
  const double timeBase = e->timeBase;
  const int ti_current = r->e->ti_current;
  const int count = c->count;
  const int gcount = c->gcount;
  struct part *restrict parts = c->parts;
  struct xpart *restrict xparts = c->xparts;
  struct gpart *restrict gparts = c->gparts;
  const double const_G = r->e->physical_constants->const_newton_G;

  TIMER_TIC;

  /* Anything to do here? */
  if (c->ti_end_min > ti_current) {
    c->updated = 0;
    c->g_updated = 0;
    return;
  }

#ifdef TASK_VERBOSE
  OUT;
#endif

  int updated = 0, g_updated = 0;
  int ti_end_min = max_nr_timesteps, ti_end_max = 0;

  /* No children? */
  if (!c->split) {

    /* Loop over the g-particles and kick the active ones. */
    for (int k = 0; k < gcount; k++) {

      /* Get a handle on the part. */
      struct gpart *restrict gp = &gparts[k];

      /* If the g-particle has no counterpart and needs to be kicked */
      if (gp->id_or_neg_offset > 0) {

        if (gp->ti_end <= ti_current) {

          /* First, finish the force calculation */
          gravity_end_force(gp, const_G);

          /* Compute the next timestep */
          const int new_dti = get_gpart_timestep(gp, e);

          /* Now we have a time step, proceed with the kick */
          kick_gpart(gp, new_dti, timeBase);

          /* Number of updated g-particles */
          g_updated++;
        }

        /* Minimal time for next end of time-step */
        ti_end_min = min(gp->ti_end, ti_end_min);
        ti_end_max = max(gp->ti_end, ti_end_max);
      }
    }

    /* Now do the hydro ones... */

    /* Loop over the particles and kick the active ones. */
    for (int k = 0; k < count; k++) {

      /* Get a handle on the part. */
      struct part *restrict p = &parts[k];
      struct xpart *restrict xp = &xparts[k];

      /* If particle needs to be kicked */
      if (p->ti_end <= ti_current) {

        /* First, finish the force loop */
        hydro_end_force(p);
        if (p->gpart != NULL) gravity_end_force(p->gpart, const_G);

        /* Compute the next timestep (hydro condition) */
        const int new_dti = get_part_timestep(p, xp, e);

        /* Now we have a time step, proceed with the kick */
        kick_part(p, xp, new_dti, timeBase);

        /* Number of updated particles */
        updated++;
        if (p->gpart != NULL) g_updated++;
      }

      /* Minimal time for next end of time-step */
      ti_end_min = min(p->ti_end, ti_end_min);
      ti_end_max = max(p->ti_end, ti_end_max);
    }
  }

  /* Otherwise, aggregate data from children. */
  else {

    /* Loop over the progeny. */
    for (int k = 0; k < 8; k++)
      if (c->progeny[k] != NULL) {
        struct cell *restrict cp = c->progeny[k];

        /* Recurse */
        runner_do_kick(r, cp, 0);

        /* And aggregate */
        updated += cp->updated;
        g_updated += cp->g_updated;
        ti_end_min = min(cp->ti_end_min, ti_end_min);
        ti_end_max = max(cp->ti_end_max, ti_end_max);
      }
  }

  /* Store the values. */
  c->updated = updated;
  c->g_updated = g_updated;
  c->ti_end_min = ti_end_min;
  c->ti_end_max = ti_end_max;

  if (timer) TIMER_TOC(timer_kick);
}

/**
 * @brief Construct the cell properties from the received particles
 *
 * @param r The runner thread.
 * @param c The cell.
 * @param timer Are we timing this ?
 */
void runner_do_recv_cell(struct runner *r, struct cell *c, int timer) {

  const struct part *restrict parts = c->parts;
  const struct gpart *restrict gparts = c->gparts;
  const size_t nr_parts = c->count;
  const size_t nr_gparts = c->gcount;
  // const int ti_current = r->e->ti_current;

  TIMER_TIC;

  int ti_end_min = max_nr_timesteps;
  int ti_end_max = 0;
  float h_max = 0.f;

  /* If this cell is a leaf, collect the particle data. */
  if (!c->split) {

    /* Collect everything... */
    for (size_t k = 0; k < nr_parts; k++) {
      const int ti_end = parts[k].ti_end;
      // if(ti_end < ti_current) error("Received invalid particle !");
      ti_end_min = min(ti_end_min, ti_end);
      ti_end_max = max(ti_end_max, ti_end);
      h_max = max(h_max, parts[k].h);
    }
    for (size_t k = 0; k < nr_gparts; k++) {
      const int ti_end = gparts[k].ti_end;
      // if(ti_end < ti_current) error("Received invalid particle !");
      ti_end_min = min(ti_end_min, ti_end);
      ti_end_max = max(ti_end_max, ti_end);
    }

  }

  /* Otherwise, recurse and collect. */
  else {
    for (int k = 0; k < 8; k++) {
      if (c->progeny[k] != NULL) {
        runner_do_recv_cell(r, c->progeny[k], 0);
        ti_end_min = min(ti_end_min, c->progeny[k]->ti_end_min);
        ti_end_max = max(ti_end_max, c->progeny[k]->ti_end_max);
        h_max = max(h_max, c->progeny[k]->h_max);
      }
    }
  }

  /* ... and store. */
  c->ti_end_min = ti_end_min;
  c->ti_end_max = ti_end_max;
  c->h_max = h_max;

  if (timer) TIMER_TOC(timer_dorecv_cell);
}

/**
 * @brief The #runner main thread routine.
 *
 * @param data A pointer to this thread's data.
 */
void *runner_main(void *data) {

  struct runner *r = (struct runner *)data;
  struct engine *e = r->e;
  struct scheduler *sched = &e->sched;

  /* Main loop. */
  while (1) {

    /* Wait at the barrier. */
    engine_barrier(e, r->id);

    /* Re-set the pointer to the previous task, as there is none. */
    struct task *t = NULL;
    struct task *prev = NULL;

    /* Loop while there are tasks... */
    while (1) {

      /* If there's no old task, try to get a new one. */
      if (t == NULL) {

        /* Get the task. */
        TIMER_TIC
        t = scheduler_gettask(sched, r->qid, prev);
        TIMER_TOC(timer_gettask);

        /* Did I get anything? */
        if (t == NULL) break;
      }

      /* Get the cells. */
      struct cell *ci = t->ci;
      struct cell *cj = t->cj;
      t->rid = r->cpuid;

      /* Different types of tasks... */
      switch (t->type) {
        case task_type_self:
          if (t->subtype == task_subtype_density) runner_doself1_density(r, ci);
#ifdef EXTRA_HYDRO_LOOP
          else if (t->subtype == task_subtype_gradient)
            runner_doself1_gradient(r, ci);
#endif
          else if (t->subtype == task_subtype_force)
            runner_doself2_force(r, ci);
          else if (t->subtype == task_subtype_grav)
            runner_doself_grav(r, ci, 1);
          else
            error("Unknown task subtype.");
          break;
        case task_type_pair:
          if (t->subtype == task_subtype_density)
            runner_dopair1_density(r, ci, cj);
#ifdef EXTRA_HYDRO_LOOP
          else if (t->subtype == task_subtype_gradient)
            runner_dopair1_gradient(r, ci, cj);
#endif
          else if (t->subtype == task_subtype_force)
            runner_dopair2_force(r, ci, cj);
          else if (t->subtype == task_subtype_grav)
            runner_dopair_grav(r, ci, cj, 1);
          else
            error("Unknown task subtype.");
          break;
        case task_type_sort:
          runner_do_sort(r, ci, t->flags, 1);
          break;
        case task_type_sub_self:
          if (t->subtype == task_subtype_density)
            runner_dosub_self1_density(r, ci, 1);
#ifdef EXTRA_HYDRO_LOOP
          else if (t->subtype == task_subtype_gradient)
            runner_dosub_self1_gradient(r, ci, 1);
#endif
          else if (t->subtype == task_subtype_force)
            runner_dosub_self2_force(r, ci, 1);
          else if (t->subtype == task_subtype_grav)
            runner_dosub_grav(r, ci, cj, 1);
          else
            error("Unknown task subtype.");
          break;
        case task_type_sub_pair:
          if (t->subtype == task_subtype_density)
            runner_dosub_pair1_density(r, ci, cj, t->flags, 1);
#ifdef EXTRA_HYDRO_LOOP
          else if (t->subtype == task_subtype_gradient)
            runner_dosub_pair1_gradient(r, ci, cj, t->flags, 1);
#endif
          else if (t->subtype == task_subtype_force)
            runner_dosub_pair2_force(r, ci, cj, t->flags, 1);
          else if (t->subtype == task_subtype_grav)
            runner_dosub_grav(r, ci, cj, 1);
          else
            error("Unknown task subtype.");
          break;
        case task_type_init:
          runner_do_init(r, ci, 1);
          break;
        case task_type_ghost:
          runner_do_ghost(r, ci);
          break;
#ifdef EXTRA_HYDRO_LOOP
        case task_type_extra_ghost:
          runner_do_extra_ghost(r, ci);
          break;
#endif
        case task_type_kick:
          runner_do_kick(r, ci, 1);
          break;
        case task_type_kick_fixdt:
          runner_do_kick_fixdt(r, ci, 1);
          break;
#ifdef WITH_MPI
        case task_type_send:
          if (t->subtype == task_subtype_tend) {
            free(t->buff);
          }
          break;
        case task_type_recv:
          if (t->subtype == task_subtype_tend) {
            cell_unpack_ti_ends(ci, t->buff);
            free(t->buff);
          } else {
            runner_do_recv_cell(r, ci, 1);
          }
          break;
#endif
        case task_type_grav_mm:
          runner_do_grav_mm(r, t->ci, 1);
          break;
        case task_type_grav_up:
          runner_do_grav_up(r, t->ci);
          break;
        case task_type_grav_gather_m:
          break;
        case task_type_grav_fft:
          runner_do_grav_fft(r);
          break;
        case task_type_grav_external:
          runner_do_grav_external(r, t->ci, 1);
          break;
        case task_type_cooling:
          runner_do_cooling(r, t->ci, 1);
          break;
        case task_type_sourceterms:
          runner_do_sourceterms(r, t->ci, 1);
          break;
        default:
          error("Unknown task type.");
      }

      /* We're done with this task, see if we get a next one. */
      prev = t;
      t = scheduler_done(sched, t);

    } /* main loop. */
  }

  /* Be kind, rewind. */
  return NULL;
}
