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

/* Config parameters. */
#include "../config.h"

/* Some standard headers. */
#include <float.h>
#include <limits.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* MPI headers. */
#ifdef WITH_MPI
#include <mpi.h>
#endif

#ifdef HAVE_LIBNUMA
#include <numa.h>
#endif

/* This object's header. */
#include "engine.h"

/* Local headers. */
#include "atomic.h"
#include "cell.h"
#include "clocks.h"
#include "cycle.h"
#include "debug.h"
#include "error.h"
#include "hydro.h"
#include "minmax.h"
#include "parallel_io.h"
#include "part.h"
#include "partition.h"
#include "proxy.h"
#include "runner.h"
#include "serial_io.h"
#include "single_io.h"
#include "timers.h"
#include "tools.h"
#include "units.h"
#include "version.h"

const char *engine_policy_names[15] = {"none",
                                       "rand",
                                       "steal",
                                       "keep",
                                       "block",
                                       "fix_dt",
                                       "cpu_tight",
                                       "mpi",
                                       "numa_affinity",
                                       "hydro",
                                       "self_gravity",
                                       "external_gravity",
                                       "cosmology_integration",
                                       "drift_all",
                                       "cooling"};

/** The rank of the engine as a global variable (for messages). */
int engine_rank;

/**
 * @brief Link a density/force task to a cell.
 *
 * @param e The #engine.
 * @param l A pointer to the #link, will be modified atomically.
 * @param t The #task.
 *
 * @return The new #link pointer.
 */
void engine_addlink(struct engine *e, struct link **l, struct task *t) {

  /* Get the next free link. */
  const int ind = atomic_inc(&e->nr_links);
  if (ind >= e->size_links) {
    error("Link table overflow.");
  }
  struct link *res = &e->links[ind];

  /* Set it atomically. */
  res->t = t;
  res->next = atomic_swap(l, res);
}

/**
 * @brief Generate the gravity hierarchical tasks for a hierarchy of cells -
 * i.e. all the O(Npart) tasks.
 *
 * Tasks are only created here. The dependencies will be added later on.
 *
 * @param e The #engine.
 * @param c The #cell.
 * @param gsuper The gsuper #cell.
 */
void engine_make_gravity_hierarchical_tasks(struct engine *e, struct cell *c,
                                            struct cell *gsuper) {

  struct scheduler *s = &e->sched;
  const int is_with_external_gravity =
      (e->policy & engine_policy_external_gravity) ==
      engine_policy_external_gravity;
  const int is_fixdt = (e->policy & engine_policy_fixdt) == engine_policy_fixdt;

  /* Is this the super-cell? */
  if (gsuper == NULL && (c->grav != NULL || (!c->split && c->gcount > 0))) {

    /* This is the super cell, i.e. the first with gravity tasks attached. */
    gsuper = c;

    /* Local tasks only... */
    if (c->nodeID == e->nodeID) {

      /* Add the init task. */
      if (c->init == NULL)
        c->init = scheduler_addtask(s, task_type_init, task_subtype_none, 0, 0,
                                    c, NULL, 0);

      /* Add the kick task that matches the policy. */
      if (is_fixdt) {
        if (c->kick == NULL)
          c->kick = scheduler_addtask(s, task_type_kick_fixdt,
                                      task_subtype_none, 0, 0, c, NULL, 0);
      } else {
        if (c->kick == NULL)
          c->kick = scheduler_addtask(s, task_type_kick, task_subtype_none, 0,
                                      0, c, NULL, 0);
      }

      if (is_with_external_gravity)
        c->grav_external = scheduler_addtask(
            s, task_type_grav_external, task_subtype_none, 0, 0, c, NULL, 0);
    }
  }

  /* Set the super-cell. */
  c->gsuper = gsuper;

  /* Recurse. */
  if (c->split)
    for (int k = 0; k < 8; k++)
      if (c->progeny[k] != NULL)
        engine_make_gravity_hierarchical_tasks(e, c->progeny[k], gsuper);
}

/**
 * @brief Generate the hydro hierarchical tasks for a hierarchy of cells -
 * i.e. all the O(Npart) tasks.
 *
 * Tasks are only created here. The dependencies will be added later on.
 *
 * @param e The #engine.
 * @param c The #cell.
 * @param super The super #cell.
 */
void engine_make_hydro_hierarchical_tasks(struct engine *e, struct cell *c,
                                          struct cell *super) {

  struct scheduler *s = &e->sched;
  const int is_fixdt = (e->policy & engine_policy_fixdt) == engine_policy_fixdt;
  const int is_with_cooling =
      (e->policy & engine_policy_cooling) == engine_policy_cooling;

  /* Is this the super-cell? */
  if (super == NULL && (c->density != NULL || (c->count > 0 && !c->split))) {

    /* This is the super cell, i.e. the first with density tasks attached. */
    super = c;

    /* Local tasks only... */
    if (c->nodeID == e->nodeID) {

      /* Add the init task. */
      if (c->init == NULL)
        c->init = scheduler_addtask(s, task_type_init, task_subtype_none, 0, 0,
                                    c, NULL, 0);

      /* Add the kick task that matches the policy. */
      if (is_fixdt) {
        if (c->kick == NULL)
          c->kick = scheduler_addtask(s, task_type_kick_fixdt,
                                      task_subtype_none, 0, 0, c, NULL, 0);
      } else {
        if (c->kick == NULL)
          c->kick = scheduler_addtask(s, task_type_kick, task_subtype_none, 0,
                                      0, c, NULL, 0);
      }

      /* Generate the ghost task. */
      c->ghost = scheduler_addtask(s, task_type_ghost, task_subtype_none, 0, 0,
                                   c, NULL, 0);

#ifdef EXTRA_HYDRO_LOOP
      /* Generate the extra ghost task. */
      c->extra_ghost = scheduler_addtask(s, task_type_extra_ghost,
                                         task_subtype_none, 0, 0, c, NULL, 0);
#endif

      if (is_with_cooling)
        c->cooling = scheduler_addtask(s, task_type_cooling, task_subtype_none,
                                       0, 0, c, NULL, 0);
    }
  }

  /* Set the super-cell. */
  c->super = super;

  /* Recurse. */
  if (c->split)
    for (int k = 0; k < 8; k++)
      if (c->progeny[k] != NULL)
        engine_make_hydro_hierarchical_tasks(e, c->progeny[k], super);
}

/**
 * @brief Redistribute the particles amongst the nodes according
 *      to their cell's node IDs.
 *
 * The strategy here is as follows:
 * 1) Each node counts the number of particles it has to send to each other
 * node.
 * 2) The number of particles of each type is then exchanged.
 * 3) The particles to send are placed in a temporary buffer in which the
 * part-gpart links are preserved.
 * 4) Each node allocates enough space for the new particles.
 * 5) (Asynchronous) communications are issued to transfer the data.
 *
 *
 * @param e The #engine.
 */
void engine_redistribute(struct engine *e) {

#ifdef WITH_MPI

  const int nr_nodes = e->nr_nodes;
  const int nodeID = e->nodeID;
  struct space *s = e->s;
  struct cell *cells = s->cells_top;
  const int nr_cells = s->nr_cells;
  const int *cdim = s->cdim;
  const double iwidth[3] = {s->iwidth[0], s->iwidth[1], s->iwidth[2]};
  const double dim[3] = {s->dim[0], s->dim[1], s->dim[2]};
  struct part *parts = s->parts;
  struct xpart *xparts = s->xparts;
  struct gpart *gparts = s->gparts;
  ticks tic = getticks();

  /* Allocate temporary arrays to store the counts of particles to be sent
     and the destination of each particle */
  int *counts, *g_counts;
  if ((counts = (int *)malloc(sizeof(int) * nr_nodes * nr_nodes)) == NULL)
    error("Failed to allocate count temporary buffer.");
  if ((g_counts = (int *)malloc(sizeof(int) * nr_nodes * nr_nodes)) == NULL)
    error("Failed to allocate gcount temporary buffer.");
  bzero(counts, sizeof(int) * nr_nodes * nr_nodes);
  bzero(g_counts, sizeof(int) * nr_nodes * nr_nodes);

  /* Allocate the destination index arrays. */
  int *dest, *g_dest;
  if ((dest = (int *)malloc(sizeof(int) * s->nr_parts)) == NULL)
    error("Failed to allocate dest temporary buffer.");
  if ((g_dest = (int *)malloc(sizeof(int) * s->nr_gparts)) == NULL)
    error("Failed to allocate g_dest temporary buffer.");

  /* Get destination of each particle */
  for (size_t k = 0; k < s->nr_parts; k++) {

    /* Periodic boundary conditions */
    for (int j = 0; j < 3; j++) {
      if (parts[k].x[j] < 0.0)
        parts[k].x[j] += dim[j];
      else if (parts[k].x[j] >= dim[j])
        parts[k].x[j] -= dim[j];
    }
    const int cid =
        cell_getid(cdim, parts[k].x[0] * iwidth[0], parts[k].x[1] * iwidth[1],
                   parts[k].x[2] * iwidth[2]);
#ifdef SWIFT_DEBUG_CHECKS
    if (cid < 0 || cid >= s->nr_cells)
      error("Bad cell id %i for part %zu at [%.3e,%.3e,%.3e].", cid, k,
            parts[k].x[0], parts[k].x[1], parts[k].x[2]);
#endif

    dest[k] = cells[cid].nodeID;

    /* The counts array is indexed as count[from * nr_nodes + to]. */
    counts[nodeID * nr_nodes + dest[k]] += 1;
  }

  /* Sort the particles according to their cell index. */
  space_parts_sort(s, dest, s->nr_parts, 0, nr_nodes - 1, e->verbose);

  /* We need to re-link the gpart partners of parts. */
  if (s->nr_parts > 0) {
    int current_dest = dest[0];
    size_t count_this_dest = 0;
    for (size_t k = 0; k < s->nr_parts; ++k) {
      if (s->parts[k].gpart != NULL) {

        /* As the addresses will be invalidated by the communications, we will
         * instead store the absolute index from the start of the sub-array of
         * particles to be sent to a given node.
         * Recall that gparts without partners have a negative id.
         * We will restore the pointers on the receiving node later on. */
        if (dest[k] != current_dest) {
          current_dest = dest[k];
          count_this_dest = 0;
        }

#ifdef SWIFT_DEBUG_CHECKS
        if (s->parts[k].gpart->id_or_neg_offset >= 0)
          error("Trying to link a partnerless gpart !");
#endif

        s->parts[k].gpart->id_or_neg_offset = -count_this_dest;
        count_this_dest++;
      }
    }
  }

  /* Get destination of each g-particle */
  for (size_t k = 0; k < s->nr_gparts; k++) {

    /* Periodic boundary conditions */
    for (int j = 0; j < 3; j++) {
      if (gparts[k].x[j] < 0.0)
        gparts[k].x[j] += dim[j];
      else if (gparts[k].x[j] >= dim[j])
        gparts[k].x[j] -= dim[j];
    }
    const int cid =
        cell_getid(cdim, gparts[k].x[0] * iwidth[0], gparts[k].x[1] * iwidth[1],
                   gparts[k].x[2] * iwidth[2]);
#ifdef SWIFT_DEBUG_CHECKS
    if (cid < 0 || cid >= s->nr_cells)
      error("Bad cell id %i for part %zu at [%.3e,%.3e,%.3e].", cid, k,
            gparts[k].x[0], gparts[k].x[1], gparts[k].x[2]);
#endif

    g_dest[k] = cells[cid].nodeID;

    /* The counts array is indexed as count[from * nr_nodes + to]. */
    g_counts[nodeID * nr_nodes + g_dest[k]] += 1;
  }

  /* Sort the gparticles according to their cell index. */
  space_gparts_sort(s, g_dest, s->nr_gparts, 0, nr_nodes - 1, e->verbose);

  /* Get all the counts from all the nodes. */
  if (MPI_Allreduce(MPI_IN_PLACE, counts, nr_nodes * nr_nodes, MPI_INT, MPI_SUM,
                    MPI_COMM_WORLD) != MPI_SUCCESS)
    error("Failed to allreduce particle transfer counts.");

  /* Get all the g_counts from all the nodes. */
  if (MPI_Allreduce(MPI_IN_PLACE, g_counts, nr_nodes * nr_nodes, MPI_INT,
                    MPI_SUM, MPI_COMM_WORLD) != MPI_SUCCESS)
    error("Failed to allreduce gparticle transfer counts.");

  /* Each node knows how many parts and gparts will be transferred to every
     other node. We can start preparing to receive data */

  /* Get the new number of parts and gparts for this node */
  size_t nr_parts = 0, nr_gparts = 0;
  for (int k = 0; k < nr_nodes; k++) nr_parts += counts[k * nr_nodes + nodeID];
  for (int k = 0; k < nr_nodes; k++)
    nr_gparts += g_counts[k * nr_nodes + nodeID];

  /* Allocate the new arrays with some extra margin */
  struct part *parts_new = NULL;
  struct xpart *xparts_new = NULL;
  struct gpart *gparts_new = NULL;
  if (posix_memalign((void **)&parts_new, part_align,
                     sizeof(struct part) * nr_parts *
                         engine_redistribute_alloc_margin) != 0)
    error("Failed to allocate new part data.");
  if (posix_memalign((void **)&xparts_new, xpart_align,
                     sizeof(struct xpart) * nr_parts *
                         engine_redistribute_alloc_margin) != 0)
    error("Failed to allocate new xpart data.");
  if (posix_memalign((void **)&gparts_new, gpart_align,
                     sizeof(struct gpart) * nr_gparts *
                         engine_redistribute_alloc_margin) != 0)
    error("Failed to allocate new gpart data.");

  /* Prepare MPI requests for the asynchronous communications */
  MPI_Request *reqs;
  if ((reqs = (MPI_Request *)malloc(sizeof(MPI_Request) * 6 * nr_nodes)) ==
      NULL)
    error("Failed to allocate MPI request list.");
  for (int k = 0; k < 6 * nr_nodes; k++) reqs[k] = MPI_REQUEST_NULL;

  /* Emit the sends and recvs for the particle and gparticle data. */
  size_t offset_send = 0, offset_recv = 0;
  size_t g_offset_send = 0, g_offset_recv = 0;
  for (int k = 0; k < nr_nodes; k++) {

    /* Indices in the count arrays of the node of interest */
    const int ind_send = nodeID * nr_nodes + k;
    const int ind_recv = k * nr_nodes + nodeID;

    /* Are we sending any part/xpart ? */
    if (counts[ind_send] > 0) {

      /* message("Sending %d part to node %d", counts[ind_send], k); */

      /* If the send is to the same node, just copy */
      if (k == nodeID) {
        memcpy(&parts_new[offset_recv], &s->parts[offset_send],
               sizeof(struct part) * counts[ind_recv]);
        memcpy(&xparts_new[offset_recv], &s->xparts[offset_send],
               sizeof(struct xpart) * counts[ind_recv]);
        offset_send += counts[ind_send];
        offset_recv += counts[ind_recv];

        /* Else, emit some communications */
      } else {
        if (MPI_Isend(&s->parts[offset_send], counts[ind_send], part_mpi_type,
                      k, 3 * ind_send + 0, MPI_COMM_WORLD,
                      &reqs[6 * k]) != MPI_SUCCESS)
          error("Failed to isend parts to node %i.", k);
        if (MPI_Isend(&s->xparts[offset_send], counts[ind_send], xpart_mpi_type,
                      k, 3 * ind_send + 1, MPI_COMM_WORLD,
                      &reqs[6 * k + 1]) != MPI_SUCCESS)
          error("Failed to isend xparts to node %i.", k);
        offset_send += counts[ind_send];
      }
    }

    /* Are we sending any gpart ? */
    if (g_counts[ind_send] > 0) {

      /* message("Sending %d gpart to node %d", g_counts[ind_send], k); */

      /* If the send is to the same node, just copy */
      if (k == nodeID) {
        memcpy(&gparts_new[g_offset_recv], &s->gparts[g_offset_send],
               sizeof(struct gpart) * g_counts[ind_recv]);
        g_offset_send += g_counts[ind_send];
        g_offset_recv += g_counts[ind_recv];

        /* Else, emit some communications */
      } else {
        if (MPI_Isend(&s->gparts[g_offset_send], g_counts[ind_send],
                      gpart_mpi_type, k, 3 * ind_send + 2, MPI_COMM_WORLD,
                      &reqs[6 * k + 2]) != MPI_SUCCESS)
          error("Failed to isend gparts to node %i.", k);
        g_offset_send += g_counts[ind_send];
      }
    }

    /* Now emit the corresponding Irecv() */

    /* Are we receiving any part/xpart from this node ? */
    if (k != nodeID && counts[ind_recv] > 0) {
      if (MPI_Irecv(&parts_new[offset_recv], counts[ind_recv], part_mpi_type, k,
                    3 * ind_recv + 0, MPI_COMM_WORLD,
                    &reqs[6 * k + 3]) != MPI_SUCCESS)
        error("Failed to emit irecv of parts from node %i.", k);
      if (MPI_Irecv(&xparts_new[offset_recv], counts[ind_recv], xpart_mpi_type,
                    k, 3 * ind_recv + 1, MPI_COMM_WORLD,
                    &reqs[6 * k + 4]) != MPI_SUCCESS)
        error("Failed to emit irecv of xparts from node %i.", k);
      offset_recv += counts[ind_recv];
    }

    /* Are we receiving any gpart from this node ? */
    if (k != nodeID && g_counts[ind_recv] > 0) {
      if (MPI_Irecv(&gparts_new[g_offset_recv], g_counts[ind_recv],
                    gpart_mpi_type, k, 3 * ind_recv + 2, MPI_COMM_WORLD,
                    &reqs[6 * k + 5]) != MPI_SUCCESS)
        error("Failed to emit irecv of gparts from node %i.", k);
      g_offset_recv += g_counts[ind_recv];
    }
  }

  /* Wait for all the sends and recvs to tumble in. */
  MPI_Status stats[6 * nr_nodes];
  int res;
  if ((res = MPI_Waitall(6 * nr_nodes, reqs, stats)) != MPI_SUCCESS) {
    for (int k = 0; k < 6 * nr_nodes; k++) {
      char buff[MPI_MAX_ERROR_STRING];
      MPI_Error_string(stats[k].MPI_ERROR, buff, &res);
      message("request %i has error '%s'.", k, buff);
    }
    error("Failed during waitall for part data.");
  }

  /* We now need to restore the part<->gpart links */
  size_t offset_parts = 0, offset_gparts = 0;
  for (int node = 0; node < nr_nodes; ++node) {

    const int ind_recv = node * nr_nodes + nodeID;
    const size_t count_parts = counts[ind_recv];
    const size_t count_gparts = g_counts[ind_recv];

    /* Loop over the gparts received from that node */
    for (size_t k = offset_gparts; k < offset_gparts + count_gparts; ++k) {

      /* Does this gpart have a partner ? */
      if (gparts_new[k].id_or_neg_offset <= 0) {

        const ptrdiff_t partner_index =
            offset_parts - gparts_new[k].id_or_neg_offset;

        /* Re-link */
        gparts_new[k].id_or_neg_offset = -partner_index;
        parts_new[partner_index].gpart = &gparts_new[k];
      }
    }

    offset_parts += count_parts;
    offset_gparts += count_gparts;
  }

#ifdef SWIFT_DEBUG_CHECKS
  /* Verify that all parts are in the right place. */
  for (size_t k = 0; k < nr_parts; k++) {
    const int cid = cell_getid(cdim, parts_new[k].x[0] * iwidth[0],
                               parts_new[k].x[1] * iwidth[1],
                               parts_new[k].x[2] * iwidth[2]);
    if (cells[cid].nodeID != nodeID)
      error("Received particle (%zu) that does not belong here (nodeID=%i).", k,
            cells[cid].nodeID);
  }

  /* Verify that the links are correct */
  for (size_t k = 0; k < nr_gparts; ++k) {

    if (gparts_new[k].id_or_neg_offset <= 0) {

      struct part *part = &parts_new[-gparts_new[k].id_or_neg_offset];

      if (part->gpart != &gparts_new[k]) error("Linking problem !");

      if (gparts_new[k].x[0] != part->x[0] ||
          gparts_new[k].x[1] != part->x[1] || gparts_new[k].x[2] != part->x[2])
        error("Linked particles are not at the same position !");
    }
  }
  for (size_t k = 0; k < nr_parts; ++k) {

    if (parts_new[k].gpart != NULL &&
        parts_new[k].gpart->id_or_neg_offset != -(ptrdiff_t)k) {
      error("Linking problem !");
    }
  }
#endif

  /* Set the new part data, free the old. */
  free(parts);
  free(xparts);
  free(gparts);
  s->parts = parts_new;
  s->xparts = xparts_new;
  s->gparts = gparts_new;
  s->nr_parts = nr_parts;
  s->nr_gparts = nr_gparts;
  s->size_parts = engine_redistribute_alloc_margin * nr_parts;
  s->size_gparts = engine_redistribute_alloc_margin * nr_gparts;

  /* Clean up the temporary stuff. */
  free(reqs);
  free(counts);
  free(dest);

  /* Be verbose about what just happened. */
  if (e->verbose) {
    int my_cells = 0;
    for (int k = 0; k < nr_cells; k++)
      if (cells[k].nodeID == nodeID) my_cells += 1;
    message("node %i now has %zu parts and %zu gparts in %i cells.", nodeID,
            nr_parts, nr_gparts, my_cells);
  }

  if (e->verbose)
    message("took %.3f %s.", clocks_from_ticks(getticks() - tic),
            clocks_getunit());
#else
  error("SWIFT was not compiled with MPI support.");
#endif
}

/**
 * @brief Repartition the cells amongst the nodes.
 *
 * @param e The #engine.
 */
void engine_repartition(struct engine *e) {

#if defined(WITH_MPI) && defined(HAVE_METIS)

  ticks tic = getticks();

  /* Clear the repartition flag. */
  e->forcerepart = 0;

  /* Nothing to do if only using a single node. Also avoids METIS
   * bug that doesn't handle this case well. */
  if (e->nr_nodes == 1) return;

  /* Do the repartitioning. */
  partition_repartition(&e->repartdata);

  /* Clear repartition data so we accumulate until next time. */
  partition_repartition_clear(&e->repartdata);

  /* Now comes the tricky part: Exchange particles between all nodes.
     This is done in two steps, first allreducing a matrix of
     how many particles go from where to where, then re-allocating
     the parts array, and emitting the sends and receives.
     Finally, the space, tasks, and proxies need to be rebuilt. */

  /* Redistribute the particles between the nodes. */
  engine_redistribute(e);

  /* Make the proxies. */
  engine_makeproxies(e);

  /* Tell the engine it should re-build whenever possible */
  e->forcerebuild = 1;

  if (e->verbose)
    message("took %.3f %s.", clocks_from_ticks(getticks() - tic),
            clocks_getunit());
#else
  error("SWIFT was not compiled with MPI and METIS support.");
#endif
}

/**
 * @brief Add up/down gravity tasks to a cell hierarchy.
 *
 * @param e The #engine.
 * @param c The #cell
 * @param up The upward gravity #task.
 * @param down The downward gravity #task.
 */
void engine_addtasks_grav(struct engine *e, struct cell *c, struct task *up,
                          struct task *down) {

  /* Link the tasks to this cell. */
  c->grav_up = up;
  c->grav_down = down;

  /* Recurse? */
  if (c->split)
    for (int k = 0; k < 8; k++)
      if (c->progeny[k] != NULL)
        engine_addtasks_grav(e, c->progeny[k], up, down);
}

/**
 * @brief Add send tasks to a hierarchy of cells.
 *
 * @param e The #engine.
 * @param ci The sending #cell.
 * @param cj Dummy cell containing the nodeID of the receiving node.
 * @param t_xv The send_xv #task, if it has already been created.
 * @param t_rho The send_rho #task, if it has already been created.
 * @param t_gradient The send_gradient #task, if already created.
 * @param t_ti The send_ti #task, if required and has already been created.
 */
void engine_addtasks_send(struct engine *e, struct cell *ci, struct cell *cj,
                          struct task *t_xv, struct task *t_rho,
                          struct task *t_gradient, struct task *t_ti) {

#ifdef WITH_MPI
  struct link *l = NULL;
  struct scheduler *s = &e->sched;
  const int nodeID = cj->nodeID;

  /* Check if any of the density tasks are for the target node. */
  for (l = ci->density; l != NULL; l = l->next)
    if (l->t->ci->nodeID == nodeID ||
        (l->t->cj != NULL && l->t->cj->nodeID == nodeID))
      break;

  /* If so, attach send tasks. */
  if (l != NULL) {

    /* Create the tasks and their dependencies? */
    if (t_xv == NULL) {
      t_xv = scheduler_addtask(s, task_type_send, task_subtype_none,
                               4 * ci->tag, 0, ci, cj, 0);
      t_rho = scheduler_addtask(s, task_type_send, task_subtype_none,
                                4 * ci->tag + 1, 0, ci, cj, 0);
      if (!(e->policy & engine_policy_fixdt))
        t_ti = scheduler_addtask(s, task_type_send, task_subtype_tend,
                                 4 * ci->tag + 2, 0, ci, cj, 0);
#ifdef EXTRA_HYDRO_LOOP
      t_gradient = scheduler_addtask(s, task_type_send, task_subtype_none,
                                     4 * ci->tag + 3, 0, ci, cj, 0);
#endif

#ifdef EXTRA_HYDRO_LOOP

      scheduler_addunlock(s, t_gradient, ci->super->kick);

      scheduler_addunlock(s, ci->super->extra_ghost, t_gradient);

      /* The send_rho task should unlock the super-cell's extra_ghost task. */
      scheduler_addunlock(s, t_rho, ci->super->extra_ghost);

      /* The send_rho task depends on the cell's ghost task. */
      scheduler_addunlock(s, ci->super->ghost, t_rho);

      /* The send_xv task should unlock the super-cell's ghost task. */
      scheduler_addunlock(s, t_xv, ci->super->ghost);

#else
      /* The send_rho task should unlock the super-cell's kick task. */
      scheduler_addunlock(s, t_rho, ci->super->kick);

      /* The send_rho task depends on the cell's ghost task. */
      scheduler_addunlock(s, ci->super->ghost, t_rho);

      /* The send_xv task should unlock the super-cell's ghost task. */
      scheduler_addunlock(s, t_xv, ci->super->ghost);
#endif

      /* The super-cell's kick task should unlock the send_ti task. */
      if (t_ti != NULL) scheduler_addunlock(s, ci->super->kick, t_ti);
    }

    /* Add them to the local cell. */
    engine_addlink(e, &ci->send_xv, t_xv);
    engine_addlink(e, &ci->send_rho, t_rho);
#ifdef EXTRA_HYDRO_LOOP
    engine_addlink(e, &ci->send_gradient, t_gradient);
#endif
    if (t_ti != NULL) engine_addlink(e, &ci->send_ti, t_ti);
  }

  /* Recurse? */
  if (ci->split)
    for (int k = 0; k < 8; k++)
      if (ci->progeny[k] != NULL)
        engine_addtasks_send(e, ci->progeny[k], cj, t_xv, t_rho, t_gradient,
                             t_ti);

#else
  error("SWIFT was not compiled with MPI support.");
#endif
}

/**
 * @brief Add recv tasks to a hierarchy of cells.
 *
 * @param e The #engine.
 * @param c The foreign #cell.
 * @param t_xv The recv_xv #task, if it has already been created.
 * @param t_rho The recv_rho #task, if it has already been created.
 * @param t_gradient The recv_gradient #task, if it has already been created.
 * @param t_ti The recv_ti #task, if required and has already been created.
 */
void engine_addtasks_recv(struct engine *e, struct cell *c, struct task *t_xv,
                          struct task *t_rho, struct task *t_gradient,
                          struct task *t_ti) {

#ifdef WITH_MPI
  struct scheduler *s = &e->sched;

  /* Do we need to construct a recv task?
     Note that since c is a foreign cell, all its density tasks will involve
     only the current rank, and thus we don't have to check them.*/
  if (t_xv == NULL && c->density != NULL) {

    /* Create the tasks. */
    t_xv = scheduler_addtask(s, task_type_recv, task_subtype_none, 4 * c->tag,
                             0, c, NULL, 0);
    t_rho = scheduler_addtask(s, task_type_recv, task_subtype_none,
                              4 * c->tag + 1, 0, c, NULL, 0);
    if (!(e->policy & engine_policy_fixdt))
      t_ti = scheduler_addtask(s, task_type_recv, task_subtype_tend,
                               4 * c->tag + 2, 0, c, NULL, 0);
#ifdef EXTRA_HYDRO_LOOP
    t_gradient = scheduler_addtask(s, task_type_recv, task_subtype_none,
                                   4 * c->tag + 3, 0, c, NULL, 0);
#endif
  }
  c->recv_xv = t_xv;
  c->recv_rho = t_rho;
  c->recv_gradient = t_gradient;
  c->recv_ti = t_ti;

/* Add dependencies. */
#ifdef EXTRA_HYDRO_LOOP
  for (struct link *l = c->density; l != NULL; l = l->next) {
    scheduler_addunlock(s, t_xv, l->t);
    scheduler_addunlock(s, l->t, t_rho);
  }
  for (struct link *l = c->gradient; l != NULL; l = l->next) {
    scheduler_addunlock(s, t_rho, l->t);
    scheduler_addunlock(s, l->t, t_gradient);
  }
  for (struct link *l = c->force; l != NULL; l = l->next) {
    scheduler_addunlock(s, t_gradient, l->t);
    if (t_ti != NULL) scheduler_addunlock(s, l->t, t_ti);
  }
  if (c->sorts != NULL) scheduler_addunlock(s, t_xv, c->sorts);
#else
  for (struct link *l = c->density; l != NULL; l = l->next) {
    scheduler_addunlock(s, t_xv, l->t);
    scheduler_addunlock(s, l->t, t_rho);
  }
  for (struct link *l = c->force; l != NULL; l = l->next) {
    scheduler_addunlock(s, t_rho, l->t);
    if (t_ti != NULL) scheduler_addunlock(s, l->t, t_ti);
  }
  if (c->sorts != NULL) scheduler_addunlock(s, t_xv, c->sorts);
#endif

  /* Recurse? */
  if (c->split)
    for (int k = 0; k < 8; k++)
      if (c->progeny[k] != NULL)
        engine_addtasks_recv(e, c->progeny[k], t_xv, t_rho, t_gradient, t_ti);

#else
  error("SWIFT was not compiled with MPI support.");
#endif
}

/**
 * @brief Exchange cell structures with other nodes.
 *
 * @param e The #engine.
 */
void engine_exchange_cells(struct engine *e) {

#ifdef WITH_MPI

  struct space *s = e->s;
  struct cell *cells = s->cells_top;
  const int nr_cells = s->nr_cells;
  const int nr_proxies = e->nr_proxies;
  int offset[nr_cells];
  MPI_Request reqs_in[engine_maxproxies];
  MPI_Request reqs_out[engine_maxproxies];
  MPI_Status status;
  ticks tic = getticks();

  /* Run through the cells and get the size of the ones that will be sent off.
   */
  int count_out = 0;
  for (int k = 0; k < nr_cells; k++) {
    offset[k] = count_out;
    if (cells[k].sendto)
      count_out += (cells[k].pcell_size = cell_getsize(&cells[k]));
  }

  /* Allocate the pcells. */
  struct pcell *pcells;
  if ((pcells = (struct pcell *)malloc(sizeof(struct pcell) * count_out)) ==
      NULL)
    error("Failed to allocate pcell buffer.");

  /* Pack the cells. */
  cell_next_tag = 0;
  for (int k = 0; k < nr_cells; k++)
    if (cells[k].sendto) {
      cell_pack(&cells[k], &pcells[offset[k]]);
      cells[k].pcell = &pcells[offset[k]];
    }

  /* Launch the proxies. */
  for (int k = 0; k < nr_proxies; k++) {
    proxy_cells_exch1(&e->proxies[k]);
    reqs_in[k] = e->proxies[k].req_cells_count_in;
    reqs_out[k] = e->proxies[k].req_cells_count_out;
  }

  /* Wait for each count to come in and start the recv. */
  for (int k = 0; k < nr_proxies; k++) {
    int pid = MPI_UNDEFINED;
    if (MPI_Waitany(nr_proxies, reqs_in, &pid, &status) != MPI_SUCCESS ||
        pid == MPI_UNDEFINED)
      error("MPI_Waitany failed.");
    // message( "request from proxy %i has arrived." , pid );
    proxy_cells_exch2(&e->proxies[pid]);
  }

  /* Wait for all the sends to have finished too. */
  if (MPI_Waitall(nr_proxies, reqs_out, MPI_STATUSES_IGNORE) != MPI_SUCCESS)
    error("MPI_Waitall on sends failed.");

  /* Set the requests for the cells. */
  for (int k = 0; k < nr_proxies; k++) {
    reqs_in[k] = e->proxies[k].req_cells_in;
    reqs_out[k] = e->proxies[k].req_cells_out;
  }

  /* Wait for each pcell array to come in from the proxies. */
  for (int k = 0; k < nr_proxies; k++) {
    int pid = MPI_UNDEFINED;
    if (MPI_Waitany(nr_proxies, reqs_in, &pid, &status) != MPI_SUCCESS ||
        pid == MPI_UNDEFINED)
      error("MPI_Waitany failed.");
    // message( "cell data from proxy %i has arrived." , pid );
    for (int count = 0, j = 0; j < e->proxies[pid].nr_cells_in; j++)
      count += cell_unpack(&e->proxies[pid].pcells_in[count],
                           e->proxies[pid].cells_in[j], e->s);
  }

  /* Wait for all the sends to have finished too. */
  if (MPI_Waitall(nr_proxies, reqs_out, MPI_STATUSES_IGNORE) != MPI_SUCCESS)
    error("MPI_Waitall on sends failed.");

  /* Count the number of particles we need to import and re-allocate
     the buffer if needed. */
  size_t count_parts_in = 0, count_gparts_in = 0;
  for (int k = 0; k < nr_proxies; k++)
    for (int j = 0; j < e->proxies[k].nr_cells_in; j++) {
      count_parts_in += e->proxies[k].cells_in[j]->count;
      count_gparts_in += e->proxies[k].cells_in[j]->gcount;
    }
  if (count_parts_in > s->size_parts_foreign) {
    if (s->parts_foreign != NULL) free(s->parts_foreign);
    s->size_parts_foreign = 1.1 * count_parts_in;
    if (posix_memalign((void **)&s->parts_foreign, part_align,
                       sizeof(struct part) * s->size_parts_foreign) != 0)
      error("Failed to allocate foreign part data.");
  }
  if (count_gparts_in > s->size_gparts_foreign) {
    if (s->gparts_foreign != NULL) free(s->gparts_foreign);
    s->size_gparts_foreign = 1.1 * count_gparts_in;
    if (posix_memalign((void **)&s->gparts_foreign, gpart_align,
                       sizeof(struct gpart) * s->size_gparts_foreign) != 0)
      error("Failed to allocate foreign gpart data.");
  }

  /* Unpack the cells and link to the particle data. */
  struct part *parts = s->parts_foreign;
  struct gpart *gparts = s->gparts_foreign;
  for (int k = 0; k < nr_proxies; k++) {
    for (int j = 0; j < e->proxies[k].nr_cells_in; j++) {
      cell_link_parts(e->proxies[k].cells_in[j], parts);
      cell_link_gparts(e->proxies[k].cells_in[j], gparts);
      parts = &parts[e->proxies[k].cells_in[j]->count];
      gparts = &gparts[e->proxies[k].cells_in[j]->gcount];
    }
  }
  s->nr_parts_foreign = parts - s->parts_foreign;
  s->nr_gparts_foreign = gparts - s->gparts_foreign;

  /* Free the pcell buffer. */
  free(pcells);

  if (e->verbose)
    message("took %.3f %s.", clocks_from_ticks(getticks() - tic),
            clocks_getunit());

#else
  error("SWIFT was not compiled with MPI support.");
#endif
}

/**
 * @brief Exchange straying parts with other nodes.
 *
 * @param e The #engine.
 * @param offset_parts The index in the parts array as of which the foreign
 *        parts reside.
 * @param ind_part The foreign #cell ID of each part.
 * @param Npart The number of stray parts, contains the number of parts received
 *        on return.
 * @param offset_gparts The index in the gparts array as of which the foreign
 *        parts reside.
 * @param ind_gpart The foreign #cell ID of each gpart.
 * @param Ngpart The number of stray gparts, contains the number of gparts
 *        received on return.
 *
 * Note that this function does not mess-up the linkage between parts and
 * gparts, i.e. the received particles have correct linkeage.
 */
void engine_exchange_strays(struct engine *e, size_t offset_parts,
                            int *ind_part, size_t *Npart, size_t offset_gparts,
                            int *ind_gpart, size_t *Ngpart) {

#ifdef WITH_MPI

  struct space *s = e->s;
  ticks tic = getticks();

  /* Re-set the proxies. */
  for (int k = 0; k < e->nr_proxies; k++) {
    e->proxies[k].nr_parts_out = 0;
    e->proxies[k].nr_gparts_out = 0;
  }

  /* Put the parts and gparts into the corresponding proxies. */
  for (size_t k = 0; k < *Npart; k++) {
    /* Get the target node and proxy ID. */
    const int node_id = e->s->cells_top[ind_part[k]].nodeID;
    if (node_id < 0 || node_id >= e->nr_nodes)
      error("Bad node ID %i.", node_id);
    const int pid = e->proxy_ind[node_id];
    if (pid < 0) {
      error(
          "Do not have a proxy for the requested nodeID %i for part with "
          "id=%lld, x=[%e,%e,%e].",
          node_id, s->parts[offset_parts + k].id,
          s->parts[offset_parts + k].x[0], s->parts[offset_parts + k].x[1],
          s->parts[offset_parts + k].x[2]);
    }

    /* Re-link the associated gpart with the buffer offset of the part. */
    if (s->parts[offset_parts + k].gpart != NULL) {
      s->parts[offset_parts + k].gpart->id_or_neg_offset =
          -e->proxies[pid].nr_parts_out;
    }

    /* Load the part and xpart into the proxy. */
    proxy_parts_load(&e->proxies[pid], &s->parts[offset_parts + k],
                     &s->xparts[offset_parts + k], 1);
  }
  for (size_t k = 0; k < *Ngpart; k++) {
    const int node_id = e->s->cells_top[ind_gpart[k]].nodeID;
    if (node_id < 0 || node_id >= e->nr_nodes)
      error("Bad node ID %i.", node_id);
    const int pid = e->proxy_ind[node_id];
    if (pid < 0)
      error(
          "Do not have a proxy for the requested nodeID %i for part with "
          "id=%lli, x=[%e,%e,%e].",
          node_id, s->gparts[offset_parts + k].id_or_neg_offset,
          s->gparts[offset_gparts + k].x[0], s->gparts[offset_parts + k].x[1],
          s->gparts[offset_gparts + k].x[2]);
    proxy_gparts_load(&e->proxies[pid], &s->gparts[offset_gparts + k], 1);
  }

  /* Launch the proxies. */
  MPI_Request reqs_in[3 * engine_maxproxies];
  MPI_Request reqs_out[3 * engine_maxproxies];
  for (int k = 0; k < e->nr_proxies; k++) {
    proxy_parts_exch1(&e->proxies[k]);
    reqs_in[k] = e->proxies[k].req_parts_count_in;
    reqs_out[k] = e->proxies[k].req_parts_count_out;
  }

  /* Wait for each count to come in and start the recv. */
  for (int k = 0; k < e->nr_proxies; k++) {
    int pid = MPI_UNDEFINED;
    if (MPI_Waitany(e->nr_proxies, reqs_in, &pid, MPI_STATUS_IGNORE) !=
            MPI_SUCCESS ||
        pid == MPI_UNDEFINED)
      error("MPI_Waitany failed.");
    // message( "request from proxy %i has arrived." , pid );
    proxy_parts_exch2(&e->proxies[pid]);
  }

  /* Wait for all the sends to have finished too. */
  if (MPI_Waitall(e->nr_proxies, reqs_out, MPI_STATUSES_IGNORE) != MPI_SUCCESS)
    error("MPI_Waitall on sends failed.");

  /* Count the total number of incoming particles and make sure we have
     enough space to accommodate them. */
  int count_parts_in = 0;
  int count_gparts_in = 0;
  for (int k = 0; k < e->nr_proxies; k++) {
    count_parts_in += e->proxies[k].nr_parts_in;
    count_gparts_in += e->proxies[k].nr_gparts_in;
  }
  if (e->verbose) {
    message("sent out %zu/%zu parts/gparts, got %i/%i back.", *Npart, *Ngpart,
            count_parts_in, count_gparts_in);
  }
  if (offset_parts + count_parts_in > s->size_parts) {
    message("re-allocating parts array.");
    s->size_parts = (offset_parts + count_parts_in) * engine_parts_size_grow;
    struct part *parts_new = NULL;
    struct xpart *xparts_new = NULL;
    if (posix_memalign((void **)&parts_new, part_align,
                       sizeof(struct part) * s->size_parts) != 0 ||
        posix_memalign((void **)&xparts_new, xpart_align,
                       sizeof(struct xpart) * s->size_parts) != 0)
      error("Failed to allocate new part data.");
    memcpy(parts_new, s->parts, sizeof(struct part) * offset_parts);
    memcpy(xparts_new, s->xparts, sizeof(struct xpart) * offset_parts);
    free(s->parts);
    free(s->xparts);
    s->parts = parts_new;
    s->xparts = xparts_new;
    for (size_t k = 0; k < offset_parts; k++) {
      if (s->parts[k].gpart != NULL) {
        s->parts[k].gpart->id_or_neg_offset = -k;
      }
    }
  }
  if (offset_gparts + count_gparts_in > s->size_gparts) {
    message("re-allocating gparts array.");
    s->size_gparts = (offset_gparts + count_gparts_in) * engine_parts_size_grow;
    struct gpart *gparts_new = NULL;
    if (posix_memalign((void **)&gparts_new, gpart_align,
                       sizeof(struct gpart) * s->size_gparts) != 0)
      error("Failed to allocate new gpart data.");
    memcpy(gparts_new, s->gparts, sizeof(struct gpart) * offset_gparts);
    free(s->gparts);
    s->gparts = gparts_new;
    for (size_t k = 0; k < offset_gparts; k++) {
      if (s->gparts[k].id_or_neg_offset < 0) {
        s->parts[-s->gparts[k].id_or_neg_offset].gpart = &s->gparts[k];
      }
    }
  }

  /* Collect the requests for the particle data from the proxies. */
  int nr_in = 0, nr_out = 0;
  for (int k = 0; k < e->nr_proxies; k++) {
    if (e->proxies[k].nr_parts_in > 0) {
      reqs_in[3 * k] = e->proxies[k].req_parts_in;
      reqs_in[3 * k + 1] = e->proxies[k].req_xparts_in;
      nr_in += 2;
    } else {
      reqs_in[3 * k] = reqs_in[3 * k + 1] = MPI_REQUEST_NULL;
    }
    if (e->proxies[k].nr_gparts_in > 0) {
      reqs_in[3 * k + 2] = e->proxies[k].req_gparts_in;
      nr_in += 1;
    } else {
      reqs_in[3 * k + 2] = MPI_REQUEST_NULL;
    }
    if (e->proxies[k].nr_parts_out > 0) {
      reqs_out[3 * k] = e->proxies[k].req_parts_out;
      reqs_out[3 * k + 1] = e->proxies[k].req_xparts_out;
      nr_out += 2;
    } else {
      reqs_out[3 * k] = reqs_out[3 * k + 1] = MPI_REQUEST_NULL;
    }
    if (e->proxies[k].nr_gparts_out > 0) {
      reqs_out[3 * k + 2] = e->proxies[k].req_gparts_out;
      nr_out += 1;
    } else {
      reqs_out[3 * k + 2] = MPI_REQUEST_NULL;
    }
  }

  /* Wait for each part array to come in and collect the new
     parts from the proxies. */
  int count_parts = 0, count_gparts = 0;
  for (int k = 0; k < nr_in; k++) {
    int err, pid;
    if ((err = MPI_Waitany(3 * e->nr_proxies, reqs_in, &pid,
                           MPI_STATUS_IGNORE)) != MPI_SUCCESS) {
      char buff[MPI_MAX_ERROR_STRING];
      int res;
      MPI_Error_string(err, buff, &res);
      error("MPI_Waitany failed (%s).", buff);
    }
    if (pid == MPI_UNDEFINED) break;
    // message( "request from proxy %i has arrived." , pid / 3 );
    pid = 3 * (pid / 3);

    /* If all the requests for a given proxy have arrived... */
    if (reqs_in[pid + 0] == MPI_REQUEST_NULL &&
        reqs_in[pid + 1] == MPI_REQUEST_NULL &&
        reqs_in[pid + 2] == MPI_REQUEST_NULL) {
      /* Copy the particle data to the part/xpart/gpart arrays. */
      struct proxy *prox = &e->proxies[pid / 3];
      memcpy(&s->parts[offset_parts + count_parts], prox->parts_in,
             sizeof(struct part) * prox->nr_parts_in);
      memcpy(&s->xparts[offset_parts + count_parts], prox->xparts_in,
             sizeof(struct xpart) * prox->nr_parts_in);
      memcpy(&s->gparts[offset_gparts + count_gparts], prox->gparts_in,
             sizeof(struct gpart) * prox->nr_gparts_in);
      /* for (int k = offset; k < offset + count; k++)
         message(
            "received particle %lli, x=[%.3e %.3e %.3e], h=%.3e, from node %i.",
            s->parts[k].id, s->parts[k].x[0], s->parts[k].x[1],
            s->parts[k].x[2], s->parts[k].h, p->nodeID); */

      /* Re-link the gparts. */
      for (int kk = 0; kk < prox->nr_gparts_in; kk++) {
        struct gpart *gp = &s->gparts[offset_gparts + count_gparts + kk];
        if (gp->id_or_neg_offset <= 0) {
          struct part *p =
              &s->parts[offset_gparts + count_parts - gp->id_or_neg_offset];
          gp->id_or_neg_offset = s->parts - p;
          p->gpart = gp;
        }
      }

      /* Advance the counters. */
      count_parts += prox->nr_parts_in;
      count_gparts += prox->nr_gparts_in;
    }
  }

  /* Wait for all the sends to have finished too. */
  if (nr_out > 0)
    if (MPI_Waitall(3 * e->nr_proxies, reqs_out, MPI_STATUSES_IGNORE) !=
        MPI_SUCCESS)
      error("MPI_Waitall on sends failed.");

  if (e->verbose)
    message("took %.3f %s.", clocks_from_ticks(getticks() - tic),
            clocks_getunit());

  /* Return the number of harvested parts. */
  *Npart = count_parts;
  *Ngpart = count_gparts;

#else
  error("SWIFT was not compiled with MPI support.");
#endif
}

/**
 * @brief Constructs the top-level tasks for the short-range gravity
 * interactions.
 *
 * All top-cells get a self task.
 * All neighbouring pairs get a pair task.
 * All non-neighbouring pairs within a range of 6 cells get a M-M task.
 *
 * @param e The #engine.
 */
void engine_make_gravity_tasks(struct engine *e) {

  struct space *s = e->s;
  struct scheduler *sched = &e->sched;
  const int nodeID = e->nodeID;
  struct cell *cells = s->cells_top;
  const int nr_cells = s->nr_cells;

  for (int cid = 0; cid < nr_cells; ++cid) {

    struct cell *ci = &cells[cid];

    /* Skip cells without gravity particles */
    if (ci->gcount == 0) continue;

    /* Is that neighbour local ? */
    if (ci->nodeID != nodeID) continue;

    /* If the cells is local build a self-interaction */
    scheduler_addtask(sched, task_type_self, task_subtype_grav, 0, 0, ci, NULL,
                      0);

    /* Let's also build a task for all the non-neighbouring pm calculations */
    scheduler_addtask(sched, task_type_grav_mm, task_subtype_none, 0, 0, ci,
                      NULL, 0);

    for (int cjd = cid + 1; cjd < nr_cells; ++cjd) {

      struct cell *cj = &cells[cjd];

      /* Skip cells without gravity particles */
      if (cj->gcount == 0) continue;

      /* Is that neighbour local ? */
      if (cj->nodeID != nodeID) continue;

      if (cell_are_neighbours(ci, cj))
        scheduler_addtask(sched, task_type_pair, task_subtype_grav, 0, 0, ci,
                          cj, 1);
    }
  }
}

/**
 * @brief Constructs the top-level pair tasks for the first hydro loop over
 * neighbours
 *
 * Here we construct all the tasks for all possible neighbouring non-empty
 * local cells in the hierarchy. No dependencies are being added thus far.
 * Additional loop over neighbours can later be added by simply duplicating
 * all the tasks created by this function.
 *
 * @param e The #engine.
 */
void engine_make_hydroloop_tasks(struct engine *e) {

  struct space *s = e->s;
  struct scheduler *sched = &e->sched;
  const int nodeID = e->nodeID;
  const int *cdim = s->cdim;
  struct cell *cells = s->cells_top;

  /* Run through the highest level of cells and add pairs. */
  for (int i = 0; i < cdim[0]; i++) {
    for (int j = 0; j < cdim[1]; j++) {
      for (int k = 0; k < cdim[2]; k++) {

        /* Get the cell */
        const int cid = cell_getid(cdim, i, j, k);
        struct cell *ci = &cells[cid];

        /* Skip cells without hydro particles */
        if (ci->count == 0) continue;

        /* If the cells is local build a self-interaction */
        if (ci->nodeID == nodeID)
          scheduler_addtask(sched, task_type_self, task_subtype_density, 0, 0,
                            ci, NULL, 0);

        /* Now loop over all the neighbours of this cell */
        for (int ii = -1; ii < 2; ii++) {
          int iii = i + ii;
          if (!s->periodic && (iii < 0 || iii >= cdim[0])) continue;
          iii = (iii + cdim[0]) % cdim[0];
          for (int jj = -1; jj < 2; jj++) {
            int jjj = j + jj;
            if (!s->periodic && (jjj < 0 || jjj >= cdim[1])) continue;
            jjj = (jjj + cdim[1]) % cdim[1];
            for (int kk = -1; kk < 2; kk++) {
              int kkk = k + kk;
              if (!s->periodic && (kkk < 0 || kkk >= cdim[2])) continue;
              kkk = (kkk + cdim[2]) % cdim[2];

              /* Get the neighbouring cell */
              const int cjd = cell_getid(cdim, iii, jjj, kkk);
              struct cell *cj = &cells[cjd];

              /* Is that neighbour local and does it have particles ? */
              if (cid >= cjd || cj->count == 0 ||
                  (ci->nodeID != nodeID && cj->nodeID != nodeID))
                continue;

              /* Construct the pair task */
              const int sid =
                  sortlistID[(kk + 1) + 3 * ((jj + 1) + 3 * (ii + 1))];
              scheduler_addtask(sched, task_type_pair, task_subtype_density,
                                sid, 0, ci, cj, 1);
            }
          }
        }
      }
    }
  }
}

/**
 * @brief Counts the tasks associated with one cell and constructs the links
 *
 * For each hydrodynamic task, construct the links with the corresponding cell.
 * Similarly, construct the dependencies for all the sorting tasks.
 *
 * @param e The #engine.
 */
void engine_count_and_link_tasks(struct engine *e) {

  struct scheduler *sched = &e->sched;

  for (int ind = 0; ind < sched->nr_tasks; ind++) {

    struct task *t = &sched->tasks[ind];

    if (t->skip) continue;

    /* Link sort tasks together. */
    if (t->type == task_type_sort && t->ci->split)
      for (int j = 0; j < 8; j++)
        if (t->ci->progeny[j] != NULL && t->ci->progeny[j]->sorts != NULL) {
          t->ci->progeny[j]->sorts->skip = 0;
          scheduler_addunlock(sched, t->ci->progeny[j]->sorts, t);
        }

    /* Link density tasks to cells. */
    if (t->type == task_type_self) {
      atomic_inc(&t->ci->nr_tasks);
      if (t->subtype == task_subtype_density) {
        engine_addlink(e, &t->ci->density, t);
        atomic_inc(&t->ci->nr_density);
      }
    } else if (t->type == task_type_pair) {
      atomic_inc(&t->ci->nr_tasks);
      atomic_inc(&t->cj->nr_tasks);
      if (t->subtype == task_subtype_density) {
        engine_addlink(e, &t->ci->density, t);
        atomic_inc(&t->ci->nr_density);
        engine_addlink(e, &t->cj->density, t);
        atomic_inc(&t->cj->nr_density);
      }
    } else if (t->type == task_type_sub_self) {
      atomic_inc(&t->ci->nr_tasks);
      if (t->subtype == task_subtype_density) {
        engine_addlink(e, &t->ci->density, t);
        atomic_inc(&t->ci->nr_density);
      }
    } else if (t->type == task_type_sub_pair) {
      atomic_inc(&t->ci->nr_tasks);
      atomic_inc(&t->cj->nr_tasks);
      if (t->subtype == task_subtype_density) {
        engine_addlink(e, &t->ci->density, t);
        atomic_inc(&t->ci->nr_density);
        engine_addlink(e, &t->cj->density, t);
        atomic_inc(&t->cj->nr_density);
      }
    }
  }
}

/**
 * @brief Creates the dependency network for the gravity tasks of a given cell.
 *
 * @param sched The #scheduler.
 * @param gravity The gravity task to link.
 * @param c The cell.
 */
static inline void engine_make_gravity_dependencies(struct scheduler *sched,
                                                    struct task *gravity,
                                                    struct cell *c) {

  /* init --> gravity --> kick */
  scheduler_addunlock(sched, c->gsuper->init, gravity);
  scheduler_addunlock(sched, gravity, c->gsuper->kick);

  /* grav_up --> gravity ( --> kick) */
  scheduler_addunlock(sched, c->gsuper->grav_up, gravity);
}

/**
 * @brief Creates all the task dependencies for the gravity
 *
 * @param e The #engine
 */
void engine_link_gravity_tasks(struct engine *e) {

  struct scheduler *sched = &e->sched;
  const int nodeID = e->nodeID;
  const int nr_tasks = sched->nr_tasks;

  /* Add one task gathering all the multipoles */
  struct task *gather = scheduler_addtask(
      sched, task_type_grav_gather_m, task_subtype_none, 0, 0, NULL, NULL, 0);

  /* And one task performing the FFT */
  struct task *fft = scheduler_addtask(sched, task_type_grav_fft,
                                       task_subtype_none, 0, 0, NULL, NULL, 0);

  scheduler_addunlock(sched, gather, fft);

  for (int k = 0; k < nr_tasks; k++) {

    /* Get a pointer to the task. */
    struct task *t = &sched->tasks[k];

    /* Skip? */
    if (t->skip) continue;

    /* Multipole construction */
    if (t->type == task_type_grav_up) {
      scheduler_addunlock(sched, t, gather);
    }

    /* Long-range interaction */
    if (t->type == task_type_grav_mm) {

      /* Gather the multipoles --> mm interaction --> kick */
      scheduler_addunlock(sched, gather, t);
      scheduler_addunlock(sched, t, t->ci->gsuper->kick);

      /* init --> mm interaction */
      scheduler_addunlock(sched, t->ci->gsuper->init, t);
    }

    /* Self-interaction? */
    if (t->type == task_type_self && t->subtype == task_subtype_grav) {

      engine_make_gravity_dependencies(sched, t, t->ci);

    }

    /* Otherwise, pair interaction? */
    else if (t->type == task_type_pair && t->subtype == task_subtype_grav) {

      if (t->ci->nodeID == nodeID) {

        engine_make_gravity_dependencies(sched, t, t->ci);
      }

      if (t->cj->nodeID == nodeID && t->ci->gsuper != t->cj->gsuper) {

        engine_make_gravity_dependencies(sched, t, t->cj);
      }

    }

    /* Otherwise, sub-self interaction? */
    else if (t->type == task_type_sub_self && t->subtype == task_subtype_grav) {

      if (t->ci->nodeID == nodeID) {
        engine_make_gravity_dependencies(sched, t, t->ci);
      }
    }

    /* Otherwise, sub-pair interaction? */
    else if (t->type == task_type_sub_pair && t->subtype == task_subtype_grav) {

      if (t->ci->nodeID == nodeID) {

        engine_make_gravity_dependencies(sched, t, t->ci);
      }
      if (t->cj->nodeID == nodeID && t->ci->gsuper != t->cj->gsuper) {

        engine_make_gravity_dependencies(sched, t, t->cj);
      }
    }
  }
}

#ifdef EXTRA_HYDRO_LOOP

/**
 * @brief Creates the dependency network for the hydro tasks of a given cell.
 *
 * @param sched The #scheduler.
 * @param density The density task to link.
 * @param gradient The gradient task to link.
 * @param force The force task to link.
 * @param c The cell.
 */
static inline void engine_make_hydro_loops_dependencies(struct scheduler *sched,
                                                        struct task *density,
                                                        struct task *gradient,
                                                        struct task *force,
                                                        struct cell *c) {
  /* init --> density loop --> ghost --> gradient loop --> extra_ghost */
  /* extra_ghost --> force loop --> kick */
  scheduler_addunlock(sched, c->super->init, density);
  scheduler_addunlock(sched, density, c->super->ghost);
  scheduler_addunlock(sched, c->super->ghost, gradient);
  scheduler_addunlock(sched, gradient, c->super->extra_ghost);
  scheduler_addunlock(sched, c->super->extra_ghost, force);
  scheduler_addunlock(sched, force, c->super->kick);
}

#else

/**
 * @brief Creates the dependency network for the hydro tasks of a given cell.
 *
 * @param sched The #scheduler.
 * @param density The density task to link.
 * @param force The force task to link.
 * @param c The cell.
 */
static inline void engine_make_hydro_loops_dependencies(struct scheduler *sched,
                                                        struct task *density,
                                                        struct task *force,
                                                        struct cell *c) {
  /* init --> density loop --> ghost --> force loop --> kick */
  scheduler_addunlock(sched, c->super->init, density);
  scheduler_addunlock(sched, density, c->super->ghost);
  scheduler_addunlock(sched, c->super->ghost, force);
  scheduler_addunlock(sched, force, c->super->kick);
}

#endif
/**
 * @brief Duplicates the first hydro loop and construct all the
 * dependencies for the hydro part
 *
 * This is done by looping over all the previously constructed tasks
 * and adding another task involving the same cells but this time
 * corresponding to the second hydro loop over neighbours.
 * With all the relevant tasks for a given cell available, we construct
 * all the dependencies for that cell.
 *
 * @param e The #engine.
 */
void engine_make_extra_hydroloop_tasks(struct engine *e) {

  struct scheduler *sched = &e->sched;
  const int nr_tasks = sched->nr_tasks;
  const int nodeID = e->nodeID;

  for (int ind = 0; ind < nr_tasks; ind++) {
    struct task *t = &sched->tasks[ind];

    /* Skip? */
    if (t->skip) continue;

    /* Self-interaction? */
    if (t->type == task_type_self && t->subtype == task_subtype_density) {

#ifdef EXTRA_HYDRO_LOOP
      /* Start by constructing the task for the second  and third hydro loop */
      struct task *t2 = scheduler_addtask(
          sched, task_type_self, task_subtype_gradient, 0, 0, t->ci, NULL, 0);
      struct task *t3 = scheduler_addtask(
          sched, task_type_self, task_subtype_force, 0, 0, t->ci, NULL, 0);

      /* Add the link between the new loops and the cell */
      engine_addlink(e, &t->ci->gradient, t2);
      atomic_inc(&t->ci->nr_gradient);
      engine_addlink(e, &t->ci->force, t3);
      atomic_inc(&t->ci->nr_force);

      /* Now, build all the dependencies for the hydro */
      engine_make_hydro_loops_dependencies(sched, t, t2, t3, t->ci);

#else

      /* Start by constructing the task for the second hydro loop */
      struct task *t2 = scheduler_addtask(
          sched, task_type_self, task_subtype_force, 0, 0, t->ci, NULL, 0);

      /* Add the link between the new loop and the cell */
      engine_addlink(e, &t->ci->force, t2);
      atomic_inc(&t->ci->nr_force);

      /* Now, build all the dependencies for the hydro */
      engine_make_hydro_loops_dependencies(sched, t, t2, t->ci);
#endif
    }

    /* Otherwise, pair interaction? */
    else if (t->type == task_type_pair && t->subtype == task_subtype_density) {

#ifdef EXTRA_HYDRO_LOOP
      /* Start by constructing the task for the second and third hydro loop */
      struct task *t2 = scheduler_addtask(
          sched, task_type_pair, task_subtype_gradient, 0, 0, t->ci, t->cj, 0);
      struct task *t3 = scheduler_addtask(
          sched, task_type_pair, task_subtype_force, 0, 0, t->ci, t->cj, 0);

      /* Add the link between the new loop and both cells */
      engine_addlink(e, &t->ci->gradient, t2);
      atomic_inc(&t->ci->nr_gradient);
      engine_addlink(e, &t->cj->gradient, t2);
      atomic_inc(&t->cj->nr_gradient);
      engine_addlink(e, &t->ci->force, t3);
      atomic_inc(&t->ci->nr_force);
      engine_addlink(e, &t->cj->force, t3);
      atomic_inc(&t->cj->nr_force);

      /* Now, build all the dependencies for the hydro for the cells */
      /* that are local and are not descendant of the same super-cells */
      if (t->ci->nodeID == nodeID) {
        engine_make_hydro_loops_dependencies(sched, t, t2, t3, t->ci);
      }
      if (t->cj->nodeID == nodeID && t->ci->super != t->cj->super) {
        engine_make_hydro_loops_dependencies(sched, t, t2, t3, t->cj);
      }

#else

      /* Start by constructing the task for the second hydro loop */
      struct task *t2 = scheduler_addtask(
          sched, task_type_pair, task_subtype_force, 0, 0, t->ci, t->cj, 0);

      /* Add the link between the new loop and both cells */
      engine_addlink(e, &t->ci->force, t2);
      atomic_inc(&t->ci->nr_force);
      engine_addlink(e, &t->cj->force, t2);
      atomic_inc(&t->cj->nr_force);

      /* Now, build all the dependencies for the hydro for the cells */
      /* that are local and are not descendant of the same super-cells */
      if (t->ci->nodeID == nodeID) {
        engine_make_hydro_loops_dependencies(sched, t, t2, t->ci);
      }
      if (t->cj->nodeID == nodeID && t->ci->super != t->cj->super) {
        engine_make_hydro_loops_dependencies(sched, t, t2, t->cj);
      }

#endif

    }

    /* Otherwise, sub-self interaction? */
    else if (t->type == task_type_sub_self &&
             t->subtype == task_subtype_density) {

#ifdef EXTRA_HYDRO_LOOP

      /* Start by constructing the task for the second and third hydro loop */
      struct task *t2 =
          scheduler_addtask(sched, task_type_sub_self, task_subtype_gradient,
                            t->flags, 0, t->ci, t->cj, 0);
      struct task *t3 =
          scheduler_addtask(sched, task_type_sub_self, task_subtype_force,
                            t->flags, 0, t->ci, t->cj, 0);

      /* Add the link between the new loop and the cell */
      engine_addlink(e, &t->ci->gradient, t2);
      atomic_inc(&t->ci->nr_gradient);
      engine_addlink(e, &t->ci->force, t3);
      atomic_inc(&t->ci->nr_force);

      /* Now, build all the dependencies for the hydro for the cells */
      /* that are local and are not descendant of the same super-cells */
      if (t->ci->nodeID == nodeID) {
        engine_make_hydro_loops_dependencies(sched, t, t2, t3, t->ci);
      }

#else
      /* Start by constructing the task for the second hydro loop */
      struct task *t2 =
          scheduler_addtask(sched, task_type_sub_self, task_subtype_force,
                            t->flags, 0, t->ci, t->cj, 0);

      /* Add the link between the new loop and the cell */
      engine_addlink(e, &t->ci->force, t2);
      atomic_inc(&t->ci->nr_force);

      /* Now, build all the dependencies for the hydro for the cells */
      /* that are local and are not descendant of the same super-cells */
      if (t->ci->nodeID == nodeID) {
        engine_make_hydro_loops_dependencies(sched, t, t2, t->ci);
      }
#endif
    }

    /* Otherwise, sub-pair interaction? */
    else if (t->type == task_type_sub_pair &&
             t->subtype == task_subtype_density) {

#ifdef EXTRA_HYDRO_LOOP

      /* Start by constructing the task for the second and third hydro loop */
      struct task *t2 =
          scheduler_addtask(sched, task_type_sub_pair, task_subtype_gradient,
                            t->flags, 0, t->ci, t->cj, 0);
      struct task *t3 =
          scheduler_addtask(sched, task_type_sub_pair, task_subtype_force,
                            t->flags, 0, t->ci, t->cj, 0);

      /* Add the link between the new loop and both cells */
      engine_addlink(e, &t->ci->gradient, t2);
      atomic_inc(&t->ci->nr_gradient);
      engine_addlink(e, &t->cj->gradient, t2);
      atomic_inc(&t->cj->nr_gradient);
      engine_addlink(e, &t->ci->force, t3);
      atomic_inc(&t->ci->nr_force);
      engine_addlink(e, &t->cj->force, t3);
      atomic_inc(&t->cj->nr_force);

      /* Now, build all the dependencies for the hydro for the cells */
      /* that are local and are not descendant of the same super-cells */
      if (t->ci->nodeID == nodeID) {
        engine_make_hydro_loops_dependencies(sched, t, t2, t3, t->ci);
      }
      if (t->cj->nodeID == nodeID && t->ci->super != t->cj->super) {
        engine_make_hydro_loops_dependencies(sched, t, t2, t3, t->cj);
      }

#else
      /* Start by constructing the task for the second hydro loop */
      struct task *t2 =
          scheduler_addtask(sched, task_type_sub_pair, task_subtype_force,
                            t->flags, 0, t->ci, t->cj, 0);

      /* Add the link between the new loop and both cells */
      engine_addlink(e, &t->ci->force, t2);
      atomic_inc(&t->ci->nr_force);
      engine_addlink(e, &t->cj->force, t2);
      atomic_inc(&t->cj->nr_force);

      /* Now, build all the dependencies for the hydro for the cells */
      /* that are local and are not descendant of the same super-cells */
      if (t->ci->nodeID == nodeID) {
        engine_make_hydro_loops_dependencies(sched, t, t2, t->ci);
      }
      if (t->cj->nodeID == nodeID && t->ci->super != t->cj->super) {
        engine_make_hydro_loops_dependencies(sched, t, t2, t->cj);
      }
#endif
    }

    /* External gravity tasks should depend on init and unlock the kick */
    else if (t->type == task_type_grav_external) {
      scheduler_addunlock(sched, t->ci->init, t);
      scheduler_addunlock(sched, t, t->ci->kick);
    }

    /* Cooling tasks should depend on kick and does not unlock anything since
     it is the last task*/
    else if (t->type == task_type_cooling) {
      scheduler_addunlock(sched, t->ci->kick, t);
    }
  }
}

/**
 * @brief Constructs the gravity tasks building the multipoles and propagating
 *them to the children
 *
 * Correct implementation is still lacking here.
 *
 * @param e The #engine.
 */
void engine_make_gravityrecursive_tasks(struct engine *e) {

  struct space *s = e->s;
  struct scheduler *sched = &e->sched;
  const int nodeID = e->nodeID;
  const int nr_cells = s->nr_cells;
  struct cell *cells = s->cells_top;

  for (int k = 0; k < nr_cells; k++) {

    /* Only do this for local cells containing gravity particles */
    if (cells[k].nodeID == nodeID && cells[k].gcount > 0) {

      /* Create tasks at top level. */
      struct task *up =
          scheduler_addtask(sched, task_type_grav_up, task_subtype_none, 0, 0,
                            &cells[k], NULL, 0);

      struct task *down = NULL;
      /* struct task *down = */
      /*     scheduler_addtask(sched, task_type_grav_down, task_subtype_none, 0,
       * 0, */
      /*                       &cells[k], NULL, 0); */

      /* Push tasks down the cell hierarchy. */
      engine_addtasks_grav(e, &cells[k], up, down);
    }
  }
}

/**
 * @brief Fill the #space's task list.
 *
 * @param e The #engine we are working with.
 */
void engine_maketasks(struct engine *e) {

  struct space *s = e->s;
  struct scheduler *sched = &e->sched;
  struct cell *cells = s->cells_top;
  const int nr_cells = s->nr_cells;
  const ticks tic = getticks();

  /* Re-set the scheduler. */
  scheduler_reset(sched, s->tot_cells * engine_maxtaskspercell);

  /* Construct the firt hydro loop over neighbours */
  if (e->policy & engine_policy_hydro) engine_make_hydroloop_tasks(e);

  /* Add the gravity mm tasks. */
  if (e->policy & engine_policy_self_gravity) engine_make_gravity_tasks(e);

  /* Split the tasks. */
  scheduler_splittasks(sched);

  /* Allocate the list of cell-task links. The maximum number of links
     is the number of cells (s->tot_cells) times the number of neighbours (27)
     times the number of interaction types (2, density and force). */
  if (e->links != NULL) free(e->links);
#ifdef EXTRA_HYDRO_LOOP
  e->size_links = s->tot_cells * 27 * 3;
#else
  e->size_links = s->tot_cells * 27 * 2;
#endif
  if ((e->links = malloc(sizeof(struct link) * e->size_links)) == NULL)
    error("Failed to allocate cell-task links.");
  e->nr_links = 0;

  /* Add the gravity up/down tasks at the top-level cells and push them down. */
  if (e->policy & engine_policy_self_gravity)
    engine_make_gravityrecursive_tasks(e);

  /* Count the number of tasks associated with each cell and
     store the density tasks in each cell, and make each sort
     depend on the sorts of its progeny. */
  if (e->policy & engine_policy_hydro) engine_count_and_link_tasks(e);

  /* Append hierarchical tasks to each cells */
  if (e->policy & engine_policy_hydro)
    for (int k = 0; k < nr_cells; k++)
      engine_make_hydro_hierarchical_tasks(e, &cells[k], NULL);

  if ((e->policy & engine_policy_self_gravity) ||
      (e->policy & engine_policy_external_gravity))
    for (int k = 0; k < nr_cells; k++)
      engine_make_gravity_hierarchical_tasks(e, &cells[k], NULL);

  /* Run through the tasks and make force tasks for each density task.
     Each force task depends on the cell ghosts and unlocks the kick task
     of its super-cell. */
  if (e->policy & engine_policy_hydro) engine_make_extra_hydroloop_tasks(e);

  /* Add the dependencies for the self-gravity stuff */
  if (e->policy & engine_policy_self_gravity) engine_link_gravity_tasks(e);

#ifdef WITH_MPI

  /* Add the communication tasks if MPI is being used. */
  if (e->policy & engine_policy_mpi) {

    /* Loop over the proxies. */
    for (int pid = 0; pid < e->nr_proxies; pid++) {

      /* Get a handle on the proxy. */
      struct proxy *p = &e->proxies[pid];

      /* Loop through the proxy's incoming cells and add the
         recv tasks. */
      for (int k = 0; k < p->nr_cells_in; k++)
        engine_addtasks_recv(e, p->cells_in[k], NULL, NULL, NULL, NULL);

      /* Loop through the proxy's outgoing cells and add the
         send tasks. */
      for (int k = 0; k < p->nr_cells_out; k++)
        engine_addtasks_send(e, p->cells_out[k], p->cells_in[0], NULL, NULL,
                             NULL, NULL);
    }
  }
#endif

  /* Set the unlocks per task. */
  scheduler_set_unlocks(sched);

  /* Rank the tasks. */
  scheduler_ranktasks(sched);

  /* Weight the tasks. */
  scheduler_reweight(sched, e->verbose);

  /* Set the tasks age. */
  e->tasks_age = 0;

  if (e->verbose)
    message("took %.3f %s (including reweight).",
            clocks_from_ticks(getticks() - tic), clocks_getunit());
}

/**
 * @brief Mark tasks to be skipped and set the sort flags accordingly.
 *        Threadpool mapper function for fixdt version.
 *
 * @param map_data pointer to the tasks
 * @param num_elements number of tasks
 * @param extra_data pointer to int that will define if a rebuild is needed.
 */
void engine_marktasks_fixdt_mapper(void *map_data, int num_elements,
                                   void *extra_data) {
  /* Unpack the arguments. */
  struct task *tasks = (struct task *)map_data;
  int *rebuild_space = (int *)extra_data;

  for (int ind = 0; ind < num_elements; ind++) {
    struct task *t = &tasks[ind];

    /* Pair? */
    if (t->type == task_type_pair || t->type == task_type_sub_pair) {

      /* Local pointers. */
      const struct cell *ci = t->ci;
      const struct cell *cj = t->cj;

      /* Too much particle movement? */
      if (t->tight &&
          (max(ci->h_max, cj->h_max) + ci->dx_max + cj->dx_max > cj->dmin ||
           ci->dx_max > space_maxreldx * ci->h_max ||
           cj->dx_max > space_maxreldx * cj->h_max))
        *rebuild_space = 1;

    }

    /* Sort? */
    else if (t->type == task_type_sort) {

      /* If all the sorts have been done, make this task implicit. */
      if (!(t->flags & (t->flags ^ t->ci->sorted))) t->implicit = 1;
    }
  }
}

/**
 * @brief Mark any sort tasks as initially skipped.
 *        Threadpool mapper function.
 *
 * @param map_data pointer to the tasks
 * @param num_elements number of tasks
 * @param extra_data unused
 */
void engine_marktasks_sorts_mapper(void *map_data, int num_elements,
                                   void *extra_data) {
  /* Unpack the arguments. */
  struct task *tasks = (struct task *)map_data;
  for (int ind = 0; ind < num_elements; ind++) {
    struct task *t = &tasks[ind];
    if (t->type == task_type_sort) {
      t->flags = 0;
      t->skip = 1;
    }
  }
}

/**
 * @brief Mark tasks to be skipped and set the sort flags accordingly.
 *        Threadpool mapper function.
 *
 * @param map_data pointer to the tasks
 * @param num_elements number of tasks
 * @param extra_data pointer to int that will define if a rebuild is needed.
 */
void engine_marktasks_mapper(void *map_data, int num_elements,
                             void *extra_data) {
  /* Unpack the arguments. */
  struct task *tasks = (struct task *)map_data;
  const int ti_end = ((int *)extra_data)[0];
  int *rebuild_space = &((int *)extra_data)[1];

  for (int ind = 0; ind < num_elements; ind++) {
    struct task *t = &tasks[ind];

    /* Single-cell task? */
    if (t->type == task_type_self || t->type == task_type_ghost ||
        t->type == task_type_sub_self) {

      /* Set this task's skip. */
      t->skip = (t->ci->ti_end_min > ti_end);
    }

    /* Pair? */
    else if (t->type == task_type_pair || t->type == task_type_sub_pair) {

      /* Local pointers. */
      const struct cell *ci = t->ci;
      const struct cell *cj = t->cj;

      /* Too much particle movement? */
      if (t->tight &&
          (max(ci->h_max, cj->h_max) + ci->dx_max + cj->dx_max > cj->dmin ||
           ci->dx_max > space_maxreldx * ci->h_max ||
           cj->dx_max > space_maxreldx * cj->h_max))
        *rebuild_space = 1;

      /* Set this task's skip. */
      if ((t->skip = (ci->ti_end_min > ti_end && cj->ti_end_min > ti_end)) == 1)
        continue;

      /* Set the sort flags. */
      if (t->type == task_type_pair && t->subtype != task_subtype_grav) {
        if (!(ci->sorted & (1 << t->flags))) {
          atomic_or(&ci->sorts->flags, (1 << t->flags));
          ci->sorts->skip = 0;
        }
        if (!(cj->sorted & (1 << t->flags))) {
          atomic_or(&cj->sorts->flags, (1 << t->flags));
          cj->sorts->skip = 0;
        }
      }

#ifdef WITH_MPI

      /* Activate the send/recv flags. */
      if (ci->nodeID != engine_rank) {

        /* Activate the tasks to recv foreign cell ci's data. */
        ci->recv_xv->skip = 0;
        ci->recv_rho->skip = 0;
        ci->recv_ti->skip = 0;

        /* Look for the local cell cj's send tasks. */
        struct link *l = NULL;
        for (l = cj->send_xv; l != NULL && l->t->cj->nodeID != ci->nodeID;
             l = l->next)
          ;
        if (l == NULL) error("Missing link to send_xv task.");
        l->t->skip = 0;

        for (l = cj->send_rho; l != NULL && l->t->cj->nodeID != ci->nodeID;
             l = l->next)
          ;
        if (l == NULL) error("Missing link to send_rho task.");
        l->t->skip = 0;

        for (l = cj->send_ti; l != NULL && l->t->cj->nodeID != ci->nodeID;
             l = l->next)
          ;
        if (l == NULL) error("Missing link to send_ti task.");
        l->t->skip = 0;

      } else if (cj->nodeID != engine_rank) {

        /* Activate the tasks to recv foreign cell cj's data. */
        cj->recv_xv->skip = 0;
        cj->recv_rho->skip = 0;
        cj->recv_ti->skip = 0;
        /* Look for the local cell ci's send tasks. */
        struct link *l = NULL;
        for (l = ci->send_xv; l != NULL && l->t->cj->nodeID != cj->nodeID;
             l = l->next)
          ;
        if (l == NULL) error("Missing link to send_xv task.");
        l->t->skip = 0;

        for (l = ci->send_rho; l != NULL && l->t->cj->nodeID != cj->nodeID;
             l = l->next)
          ;
        if (l == NULL) error("Missing link to send_rho task.");
        l->t->skip = 0;

        for (l = ci->send_ti; l != NULL && l->t->cj->nodeID != cj->nodeID;
             l = l->next)
          ;
        if (l == NULL) error("Missing link to send_ti task.");
        l->t->skip = 0;
      }

#endif
    }

    /* Kick? */
    else if (t->type == task_type_kick) {
      t->skip = (t->ci->ti_end_min > ti_end);
      t->ci->updated = 0;
      t->ci->g_updated = 0;
    }

    /* Init? */
    else if (t->type == task_type_init) {
      /* Set this task's skip. */
      t->skip = (t->ci->ti_end_min > ti_end);
    }

    /* None? */
    else if (t->type == task_type_none)
      t->skip = 1;
  }
}

/**
 * @brief Mark tasks to be skipped and set the sort flags accordingly.
 *
 * @return 1 if the space has to be rebuilt, 0 otherwise.
 */
int engine_marktasks(struct engine *e) {

  struct scheduler *s = &e->sched;
  const ticks tic = getticks();
  int rebuild_space = 0;

  /* Much less to do here if we're on a fixed time-step. */
  if (e->policy & engine_policy_fixdt) {

    /* Run through the tasks and mark as skip or not. */
    threadpool_map(&e->threadpool, engine_marktasks_fixdt_mapper, s->tasks,
                   s->nr_tasks, sizeof(struct task), 1000, &rebuild_space);
    return rebuild_space;

    /* Multiple-timestep case */
  } else {

    /* Run through the tasks and mark as skip or not. */
    int extra_data[2] = {e->ti_current, rebuild_space};
    threadpool_map(&e->threadpool, engine_marktasks_sorts_mapper, s->tasks,
                   s->nr_tasks, sizeof(struct task), 10000, NULL);

#ifdef WITH_MPI
    if (e->policy & engine_policy_mpi) {

      /* Skip all sends and recvs, we will unmark if needed. */
      for (int k = 0; k < s->nr_tasks; k++) {
        struct task *t = &s->tasks[k];
        if (t->type == task_type_send || t->type == task_type_recv) {
          t->skip = 1;
        }
      }
    }
#endif

    threadpool_map(&e->threadpool, engine_marktasks_mapper, s->tasks,
                   s->nr_tasks, sizeof(struct task), 10000, extra_data);
    rebuild_space = extra_data[1];
  }

  if (e->verbose)
    message("took %.3f %s.", clocks_from_ticks(getticks() - tic),
            clocks_getunit());

  /* All is well... */
  return rebuild_space;
}

/**
 * @brief Prints the number of tasks in the engine
 *
 * @param e The #engine.
 */
void engine_print_task_counts(struct engine *e) {

  struct scheduler *sched = &e->sched;

  /* Count and print the number of each task type. */
  int counts[task_type_count + 1];
  for (int k = 0; k <= task_type_count; k++) counts[k] = 0;
  for (int k = 0; k < sched->nr_tasks; k++)
    if (!sched->tasks[k].skip)
      counts[(int)sched->tasks[k].type] += 1;
    else
      counts[task_type_count] += 1;
#ifdef WITH_MPI
  printf("[%04i] %s engine_print_task_counts: task counts are [ %s=%i",
         e->nodeID, clocks_get_timesincestart(), taskID_names[0], counts[0]);
#else
  printf("%s engine_print_task_counts: task counts are [ %s=%i",
         clocks_get_timesincestart(), taskID_names[0], counts[0]);
#endif
  for (int k = 1; k < task_type_count; k++)
    printf(" %s=%i", taskID_names[k], counts[k]);
  printf(" skipped=%i ]\n", counts[task_type_count]);
  fflush(stdout);
  message("nr_parts = %zu.", e->s->nr_parts);
  message("nr_gparts = %zu.", e->s->nr_gparts);
}

/**
 * @brief Rebuild the space and tasks.
 *
 * @param e The #engine.
 */
void engine_rebuild(struct engine *e) {

  const ticks tic = getticks();

  /* Clear the forcerebuild flag, whatever it was. */
  e->forcerebuild = 0;

  /* Re-build the space. */
  space_rebuild(e->s, 0.0, e->verbose);

/* If in parallel, exchange the cell structure. */
#ifdef WITH_MPI
  engine_exchange_cells(e);
#endif

  /* Re-build the tasks. */
  engine_maketasks(e);

  /* Run through the tasks and mark as skip or not. */
  if (engine_marktasks(e))
    error("engine_marktasks failed after space_rebuild.");

  /* Print the status of the system */
  if (e->verbose) engine_print_task_counts(e);

  if (e->verbose)
    message("took %.3f %s.", clocks_from_ticks(getticks() - tic),
            clocks_getunit());
}

/**
 * @brief Prepare the #engine by re-building the cells and tasks.
 *
 * @param e The #engine to prepare.
 * @param nodrift Whether to drift particles before rebuilding or not. Will
 *                not be necessary if all particles have already been
 *                drifted (before repartitioning for instance).
 */
void engine_prepare(struct engine *e, int nodrift) {

  TIMER_TIC;

  /* Run through the tasks and mark as skip or not. */
  int rebuild = (e->forcerebuild || engine_marktasks(e));

/* Collect the values of rebuild from all nodes. */
#ifdef WITH_MPI
  int buff = 0;
  if (MPI_Allreduce(&rebuild, &buff, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD) !=
      MPI_SUCCESS)
    error("Failed to aggregate the rebuild flag across nodes.");
  rebuild = buff;
#endif

  /* And rebuild if necessary. */
  if (rebuild) {

    /* Drift all particles to the current time if needed. */
    if (!nodrift) {
      e->drift_all = 1;
      engine_drift(e);

      /* Restore the default drifting policy */
      e->drift_all = (e->policy & engine_policy_drift_all);
    }

    engine_rebuild(e);
  }

  /* Re-rank the tasks every now and then. */
  if (e->tasks_age % engine_tasksreweight == 1) {
    scheduler_reweight(&e->sched, e->verbose);
  }
  e->tasks_age += 1;

  TIMER_TOC(timer_prepare);

  if (e->verbose)
    message("took %.3f %s (including marktask, rebuild and reweight).",
            clocks_from_ticks(getticks() - tic), clocks_getunit());
}

/**
 * @brief Implements a barrier for the #runner threads.
 *
 * @param e The #engine.
 * @param tid The thread ID
 */
void engine_barrier(struct engine *e, int tid) {

  /* First, get the barrier mutex. */
  if (pthread_mutex_lock(&e->barrier_mutex) != 0)
    error("Failed to get barrier mutex.");

  /* This thread is no longer running. */
  e->barrier_running -= 1;

  /* If all threads are in, send a signal... */
  if (e->barrier_running == 0)
    if (pthread_cond_broadcast(&e->barrier_cond) != 0)
      error("Failed to broadcast barrier full condition.");

  /* Wait for the barrier to open. */
  while (e->barrier_launch == 0 || tid >= e->barrier_launchcount)
    if (pthread_cond_wait(&e->barrier_cond, &e->barrier_mutex) != 0)
      error("Error waiting for barrier to close.");

  /* This thread has been launched. */
  e->barrier_running += 1;
  e->barrier_launch -= 1;

  /* If I'm the last one out, signal the condition again. */
  if (e->barrier_launch == 0)
    if (pthread_cond_broadcast(&e->barrier_cond) != 0)
      error("Failed to broadcast empty barrier condition.");

  /* Last but not least, release the mutex. */
  if (pthread_mutex_unlock(&e->barrier_mutex) != 0)
    error("Failed to get unlock the barrier mutex.");
}

/**
 * @brief Mapping function to collect the data from the kick.
 *
 * @param c A super-cell.
 */
void engine_collect_kick(struct cell *c) {

  /* Skip super-cells (Their values are already set) */
  if (c->kick != NULL) return;

  /* Counters for the different quantities. */
  int updated = 0, g_updated = 0;
  int ti_end_min = max_nr_timesteps;

  /* Only do something is the cell is non-empty */
  if (c->count != 0 || c->gcount != 0) {

    /* If this cell is not split, I'm in trouble. */
    if (!c->split) error("Cell is not split.");

    /* Collect the values from the progeny. */
    for (int k = 0; k < 8; k++) {
      struct cell *cp = c->progeny[k];
      if (cp != NULL) {

        /* Recurse */
        engine_collect_kick(cp);

        /* And update */
        ti_end_min = min(ti_end_min, cp->ti_end_min);
        updated += cp->updated;
        g_updated += cp->g_updated;
      }
    }
  }

  /* Store the collected values in the cell. */
  c->ti_end_min = ti_end_min;
  c->updated = updated;
  c->g_updated = g_updated;
}

/**
 * @brief Collects the next time-step by making each super-cell recurse
 * to collect the minimal of ti_end and the number of updated particles.
 *
 * @param e The #engine.
 */
void engine_collect_timestep(struct engine *e) {

  const ticks tic = getticks();
  int updates = 0, g_updates = 0;
  int ti_end_min = max_nr_timesteps;
  const struct space *s = e->s;

  /* Collect the cell data. */
  for (int k = 0; k < s->nr_cells; k++)
    if (s->cells_top[k].nodeID == e->nodeID) {
      struct cell *c = &s->cells_top[k];

      /* Make the top-cells recurse */
      engine_collect_kick(c);

      /* And aggregate */
      ti_end_min = min(ti_end_min, c->ti_end_min);
      updates += c->updated;
      g_updates += c->g_updated;
    }

/* Aggregate the data from the different nodes. */
#ifdef WITH_MPI
  {
    int in_i[1], out_i[1];
    in_i[0] = 0;
    out_i[0] = ti_end_min;
    if (MPI_Allreduce(out_i, in_i, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD) !=
        MPI_SUCCESS)
      error("Failed to aggregate t_end_min.");
    ti_end_min = in_i[0];
  }
  {
    unsigned long long in_ll[2], out_ll[2];
    out_ll[0] = updates;
    out_ll[1] = g_updates;
    if (MPI_Allreduce(out_ll, in_ll, 2, MPI_LONG_LONG_INT, MPI_SUM,
                      MPI_COMM_WORLD) != MPI_SUCCESS)
      error("Failed to aggregate energies.");
    updates = in_ll[0];
    g_updates = in_ll[1];
  }
#endif

  e->ti_end_min = ti_end_min;
  e->updates = updates;
  e->g_updates = g_updates;

  if (e->verbose)
    message("took %.3f %s.", clocks_from_ticks(getticks() - tic),
            clocks_getunit());
}

/**
 * @brief Print the conserved quantities statistics to a log file
 *
 * @param e The #engine.
 */
void engine_print_stats(struct engine *e) {

  const ticks tic = getticks();
  const struct space *s = e->s;

  double e_kin = 0.0, e_int = 0.0, e_pot = 0.0, e_rad = 0.0;
  double entropy = 0.0, mass = 0.0;
  double mom[3] = {0.0, 0.0, 0.0}, ang_mom[3] = {0.0, 0.0, 0.0};

  /* Collect the cell data. */
  for (int k = 0; k < s->nr_cells; k++)
    if (s->cells_top[k].nodeID == e->nodeID) {
      struct cell *c = &s->cells_top[k];
      mass += c->mass;
      e_kin += c->e_kin;
      e_int += c->e_int;
      e_pot += c->e_pot;
      e_rad += c->e_rad;
      entropy += c->entropy;
      mom[0] += c->mom[0];
      mom[1] += c->mom[1];
      mom[2] += c->mom[2];
      ang_mom[0] += c->ang_mom[0];
      ang_mom[1] += c->ang_mom[1];
      ang_mom[2] += c->ang_mom[2];
    }

/* Aggregate the data from the different nodes. */
#ifdef WITH_MPI
  {
    double in[12] = {0., 0., 0., 0., 0., 0., 0., 0., 0., 0., 0., 0.};
    double out[12];
    out[0] = e_kin;
    out[1] = e_int;
    out[2] = e_pot;
    out[3] = e_rad;
    out[4] = mom[0];
    out[5] = mom[1];
    out[6] = mom[2];
    out[7] = ang_mom[0];
    out[8] = ang_mom[1];
    out[9] = ang_mom[2];
    out[10] = mass;
    out[11] = entropy;
    if (MPI_Reduce(out, in, 11, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD) !=
        MPI_SUCCESS)
      error("Failed to aggregate stats.");
    e_kin = out[0];
    e_int = out[1];
    e_pot = out[2];
    e_rad = out[3];
    mom[0] = out[4];
    mom[1] = out[5];
    mom[2] = out[6];
    ang_mom[0] = out[7];
    ang_mom[1] = out[8];
    ang_mom[2] = out[9];
    mass = out[10];
    entropy = out[11];
  }
#endif

  const double e_tot = e_kin + e_int + e_pot;

  /* Print info */
  if (e->nodeID == 0) {
    fprintf(e->file_stats,
            " %14e %14e %14e %14e %14e %14e %14e %14e %14e %14e %14e %14e %14e "
            "%14e\n",
            e->time, mass, e_tot, e_kin, e_int, e_pot, e_rad, entropy, mom[0],
            mom[1], mom[2], ang_mom[0], ang_mom[1], ang_mom[2]);
    fflush(e->file_stats);
  }

  if (e->verbose)
    message("took %.3f %s.", clocks_from_ticks(getticks() - tic),
            clocks_getunit());
}

/**
 * @brief Launch the runners.
 *
 * @param e The #engine.
 * @param nr_runners The number of #runner to let loose.
 * @param mask The task mask to launch.
 * @param submask The sub-task mask to launch.
 */
void engine_launch(struct engine *e, int nr_runners, unsigned int mask,
                   unsigned int submask) {

  const ticks tic = getticks();

  /* Prepare the scheduler. */
  atomic_inc(&e->sched.waiting);

  /* Cry havoc and let loose the dogs of war. */
  e->barrier_launch = nr_runners;
  e->barrier_launchcount = nr_runners;
  if (pthread_cond_broadcast(&e->barrier_cond) != 0)
    error("Failed to broadcast barrier open condition.");

  /* Load the tasks. */
  pthread_mutex_unlock(&e->barrier_mutex);
  scheduler_start(&e->sched, mask, submask);
  pthread_mutex_lock(&e->barrier_mutex);

  /* Remove the safeguard. */
  pthread_mutex_lock(&e->sched.sleep_mutex);
  atomic_dec(&e->sched.waiting);
  pthread_cond_broadcast(&e->sched.sleep_cond);
  pthread_mutex_unlock(&e->sched.sleep_mutex);

  /* Sit back and wait for the runners to come home. */
  while (e->barrier_launch || e->barrier_running)
    if (pthread_cond_wait(&e->barrier_cond, &e->barrier_mutex) != 0)
      error("Error while waiting for barrier.");

  if (e->verbose)
    message("took %.3f %s.", clocks_from_ticks(getticks() - tic),
            clocks_getunit());
}

/**
 * @brief Initialises the particles and set them in a state ready to move
 *forward in time.
 *
 * @param e The #engine
 * @param flag_entropy_ICs Did the 'Internal Energy' of the particles actually
 *contain entropy ?
 */
void engine_init_particles(struct engine *e, int flag_entropy_ICs) {

  struct space *s = e->s;

  struct clocks_time time1, time2;
  clocks_gettime(&time1);

  if (e->nodeID == 0) message("Running initialisation fake time-step.");

  engine_prepare(e, 1);

  engine_marktasks(e);

  /* Build the masks corresponding to the policy */
  unsigned int mask = 0;
  unsigned int submask = 0;

  /* We always have sort tasks */
  mask |= 1 << task_type_sort;
  mask |= 1 << task_type_init;

  /* Add the tasks corresponding to hydro operations to the masks */
  if (e->policy & engine_policy_hydro) {

    mask |= 1 << task_type_self;
    mask |= 1 << task_type_pair;
    mask |= 1 << task_type_sub_self;
    mask |= 1 << task_type_sub_pair;
    mask |= 1 << task_type_ghost;

    submask |= 1 << task_subtype_density;
  }

  /* Add the tasks corresponding to self-gravity to the masks */
  if (e->policy & engine_policy_self_gravity) {

    mask |= 1 << task_type_grav_up;
    mask |= 1 << task_type_grav_mm;
    mask |= 1 << task_type_grav_gather_m;
    mask |= 1 << task_type_grav_fft;
    mask |= 1 << task_type_self;
    mask |= 1 << task_type_pair;
    mask |= 1 << task_type_sub_self;
    mask |= 1 << task_type_sub_pair;

    submask |= 1 << task_subtype_grav;
  }

  /* Add the tasks corresponding to external gravity to the masks */
  if (e->policy & engine_policy_external_gravity) {

    mask |= 1 << task_type_grav_external;
  }

  /* Add MPI tasks if need be */
  if (e->policy & engine_policy_mpi) {

    mask |= 1 << task_type_send;
    mask |= 1 << task_type_recv;
    submask |= 1 << task_subtype_tend;
  }

  /* Now, launch the calculation */
  TIMER_TIC;
  engine_launch(e, e->nr_threads, mask, submask);
  TIMER_TOC(timer_runners);

  /* Apply some conversions (e.g. internal energy -> entropy) */
  if (!flag_entropy_ICs) {

    /* Apply the conversion */
    space_map_cells_pre(s, 0, cell_convert_hydro, NULL);

    /* Correct what we did (e.g. in PE-SPH, need to recompute rho_bar) */
    if (hydro_need_extra_init_loop)
      engine_launch(e, e->nr_threads, mask, submask);
  }

  clocks_gettime(&time2);

  /* Ready to go */
  e->step = -1;
  e->wallclock_time = (float)clocks_diff(&time1, &time2);

  if (e->verbose) message("took %.3f %s.", e->wallclock_time, clocks_getunit());
}

/**
 * @brief Let the #engine loose to compute the forces.
 *
 * @param e The #engine.
 */
void engine_step(struct engine *e) {

  double snapshot_drift_time = 0.;

  TIMER_TIC2;

  struct clocks_time time1, time2;
  clocks_gettime(&time1);

  e->tic_step = getticks();

  /* Recover the (integer) end of the next time-step */
  engine_collect_timestep(e);

  /* Check for output */
  while (e->ti_end_min >= e->ti_nextSnapshot && e->ti_nextSnapshot > 0) {

    e->ti_old = e->ti_current;
    e->ti_current = e->ti_nextSnapshot;
    e->time = e->ti_current * e->timeBase + e->timeBegin;
    e->timeOld = e->ti_old * e->timeBase + e->timeBegin;
    e->timeStep = (e->ti_current - e->ti_old) * e->timeBase;
    snapshot_drift_time = e->timeStep;

    /* Drift everybody to the snapshot position */
    e->drift_all = 1;
    engine_drift(e);

    /* Restore the default drifting policy */
    e->drift_all = (e->policy & engine_policy_drift_all);

    /* Dump... */
    engine_dump_snapshot(e);

    /* ... and find the next output time */
    engine_compute_next_snapshot_time(e);
  }

  /* Move forward in time */
  e->ti_old = e->ti_current;
  e->ti_current = e->ti_end_min;
  e->step += 1;
  e->time = e->ti_current * e->timeBase + e->timeBegin;
  e->timeOld = e->ti_old * e->timeBase + e->timeBegin;
  e->timeStep = (e->ti_current - e->ti_old) * e->timeBase + snapshot_drift_time;

  if (e->nodeID == 0) {

    /* Print some information to the screen */
    printf("  %6d %14e %14e %10zu %10zu %21.3f\n", e->step, e->time,
           e->timeStep, e->updates, e->g_updates, e->wallclock_time);
    fflush(stdout);

    fprintf(e->file_timesteps, "  %6d %14e %14e %10zu %10zu %21.3f\n", e->step,
            e->time, e->timeStep, e->updates, e->g_updates, e->wallclock_time);
    fflush(e->file_timesteps);
  }

  /* Save some statistics */
  if (e->time - e->timeLastStatistics >= e->deltaTimeStatistics) {
    engine_print_stats(e);
    e->timeLastStatistics += e->deltaTimeStatistics;
  }

  /* Drift only the necessary particles, that all means all particles
   * if we are about to repartition. */
  e->drift_all = e->forcerepart || e->drift_all;
  engine_drift(e);

  /* Re-distribute the particles amongst the nodes? */
  if (e->forcerepart) {
    engine_repartition(e);
  }
  else if (e->reparttype != REPART_NONE) {

    /* We accumulate the task weights. */
    partition_repartition_accumulate(e->reparttype, e->nodeID, e->nr_nodes,
                                     e->s, e->sched.tasks, e->sched.nr_tasks,
                                     &e->repartdata);
  }

  /* Prepare the space. */
  engine_prepare(e, e->drift_all);

  /* Restore the default drifting policy */
  e->drift_all = (e->policy & engine_policy_drift_all);

  /* Build the masks corresponding to the policy */
  unsigned int mask = 0, submask = 0;

  /* We always have sort tasks and init tasks */
  mask |= 1 << task_type_sort;
  mask |= 1 << task_type_init;

  /* Add the correct kick task */
  if (e->policy & engine_policy_fixdt) {
    mask |= 1 << task_type_kick_fixdt;
  } else {
    mask |= 1 << task_type_kick;
  }

  /* Add the tasks corresponding to hydro operations to the masks */
  if (e->policy & engine_policy_hydro) {

    mask |= 1 << task_type_self;
    mask |= 1 << task_type_pair;
    mask |= 1 << task_type_sub_self;
    mask |= 1 << task_type_sub_pair;
    mask |= 1 << task_type_ghost;

    submask |= 1 << task_subtype_density;
    submask |= 1 << task_subtype_force;

#ifdef EXTRA_HYDRO_LOOP
    mask |= 1 << task_type_extra_ghost;
    submask |= 1 << task_subtype_gradient;
#endif
  }

  /* Add the tasks corresponding to self-gravity to the masks */
  if (e->policy & engine_policy_self_gravity) {

    mask |= 1 << task_type_grav_up;
    mask |= 1 << task_type_grav_mm;
    mask |= 1 << task_type_grav_gather_m;
    mask |= 1 << task_type_grav_fft;
    mask |= 1 << task_type_self;
    mask |= 1 << task_type_pair;
    mask |= 1 << task_type_sub_self;
    mask |= 1 << task_type_sub_pair;

    submask |= 1 << task_subtype_grav;
  }

  /* Add the tasks corresponding to external gravity to the masks */
  if (e->policy & engine_policy_external_gravity) {
    mask |= 1 << task_type_grav_external;
  }

  /* Add the tasks corresponding to cooling to the masks */
  if (e->policy & engine_policy_cooling) {
    mask |= 1 << task_type_cooling;
  }

  /* Add MPI tasks if need be */
  if (e->policy & engine_policy_mpi) {

    mask |= 1 << task_type_send;
    mask |= 1 << task_type_recv;
    submask |= 1 << task_subtype_tend;
  }

  if (e->verbose) engine_print_task_counts(e);

  /* Send off the runners. */
  TIMER_TIC;
  engine_launch(e, e->nr_threads, mask, submask);
  TIMER_TOC(timer_runners);

  TIMER_TOC2(timer_step);

  clocks_gettime(&time2);

  e->wallclock_time = (float)clocks_diff(&time1, &time2);
  e->toc_step = getticks();
}

/**
 * @brief Returns 1 if the simulation has reached its end point, 0 otherwise
 */
int engine_is_done(struct engine *e) {
  return !(e->ti_current < max_nr_timesteps);
}

/**
 * @brief Drift particles using the current engine drift policy.
 *
 * @param e The #engine.
 */
void engine_drift(struct engine *e) {

  const ticks tic = getticks();
  threadpool_map(&e->threadpool, runner_do_drift_mapper, e->s->cells_top,
                 e->s->nr_cells, sizeof(struct cell), 1, e);
  if (e->verbose)
    message("took %.3f %s.", clocks_from_ticks(getticks() - tic),
            clocks_getunit());
}

/**
 * @brief Create and fill the proxies.
 *
 * @param e The #engine.
 */
void engine_makeproxies(struct engine *e) {

#ifdef WITH_MPI
  const int *cdim = e->s->cdim;
  const struct space *s = e->s;
  struct cell *cells = s->cells_top;
  struct proxy *proxies = e->proxies;
  ticks tic = getticks();

  /* Prepare the proxies and the proxy index. */
  if (e->proxy_ind == NULL)
    if ((e->proxy_ind = (int *)malloc(sizeof(int) * e->nr_nodes)) == NULL)
      error("Failed to allocate proxy index.");
  for (int k = 0; k < e->nr_nodes; k++) e->proxy_ind[k] = -1;
  e->nr_proxies = 0;

  /* The following loop is super-clunky, but it's necessary
     to ensure that the order of the send and recv cells in
     the proxies is identical for all nodes! */

  /* Loop over each cell in the space. */
  int ind[3];
  for (ind[0] = 0; ind[0] < cdim[0]; ind[0]++)
    for (ind[1] = 0; ind[1] < cdim[1]; ind[1]++)
      for (ind[2] = 0; ind[2] < cdim[2]; ind[2]++) {

        /* Get the cell ID. */
        const int cid = cell_getid(cdim, ind[0], ind[1], ind[2]);

        /* Loop over all its neighbours (periodic). */
        for (int i = -1; i <= 1; i++) {
          int ii = ind[0] + i;
          if (ii >= cdim[0])
            ii -= cdim[0];
          else if (ii < 0)
            ii += cdim[0];
          for (int j = -1; j <= 1; j++) {
            int jj = ind[1] + j;
            if (jj >= cdim[1])
              jj -= cdim[1];
            else if (jj < 0)
              jj += cdim[1];
            for (int k = -1; k <= 1; k++) {
              int kk = ind[2] + k;
              if (kk >= cdim[2])
                kk -= cdim[2];
              else if (kk < 0)
                kk += cdim[2];

              /* Get the cell ID. */
              const int cjd = cell_getid(cdim, ii, jj, kk);

              /* Add to proxies? */
              if (cells[cid].nodeID == e->nodeID &&
                  cells[cjd].nodeID != e->nodeID) {
                int pid = e->proxy_ind[cells[cjd].nodeID];
                if (pid < 0) {
                  if (e->nr_proxies == engine_maxproxies)
                    error("Maximum number of proxies exceeded.");
                  proxy_init(&proxies[e->nr_proxies], e->nodeID,
                             cells[cjd].nodeID);
                  e->proxy_ind[cells[cjd].nodeID] = e->nr_proxies;
                  pid = e->nr_proxies;
                  e->nr_proxies += 1;
                }
                proxy_addcell_in(&proxies[pid], &cells[cjd]);
                proxy_addcell_out(&proxies[pid], &cells[cid]);
                cells[cid].sendto |= (1ULL << pid);
              }

              if (cells[cjd].nodeID == e->nodeID &&
                  cells[cid].nodeID != e->nodeID) {
                int pid = e->proxy_ind[cells[cid].nodeID];
                if (pid < 0) {
                  if (e->nr_proxies == engine_maxproxies)
                    error("Maximum number of proxies exceeded.");
                  proxy_init(&proxies[e->nr_proxies], e->nodeID,
                             cells[cid].nodeID);
                  e->proxy_ind[cells[cid].nodeID] = e->nr_proxies;
                  pid = e->nr_proxies;
                  e->nr_proxies += 1;
                }
                proxy_addcell_in(&proxies[pid], &cells[cid]);
                proxy_addcell_out(&proxies[pid], &cells[cjd]);
                cells[cjd].sendto |= (1ULL << pid);
              }
            }
          }
        }
      }

  if (e->verbose)
    message("took %.3f %s.", clocks_from_ticks(getticks() - tic),
            clocks_getunit());
#else
  error("SWIFT was not compiled with MPI support.");
#endif
}

/**
 * @brief Split the underlying space into regions and assign to separate nodes.
 *
 * @param e The #engine.
 * @param initial_partition structure defining the cell partition technique
 */
void engine_split(struct engine *e, struct partition *initial_partition) {

#ifdef WITH_MPI
  struct space *s = e->s;

  /* Do the initial partition of the cells. */
  partition_initial_partition(initial_partition, e->nodeID, e->nr_nodes, s);

  /* Make the proxies. */
  engine_makeproxies(e);

  /* Re-allocate the local parts. */
  if (e->verbose)
    message("Re-allocating parts array from %zu to %zu.", s->size_parts,
            (size_t)(s->nr_parts * 1.2));
  s->size_parts = s->nr_parts * 1.2;
  struct part *parts_new = NULL;
  struct xpart *xparts_new = NULL;
  if (posix_memalign((void **)&parts_new, part_align,
                     sizeof(struct part) * s->size_parts) != 0 ||
      posix_memalign((void **)&xparts_new, xpart_align,
                     sizeof(struct xpart) * s->size_parts) != 0)
    error("Failed to allocate new part data.");
  memcpy(parts_new, s->parts, sizeof(struct part) * s->nr_parts);
  memcpy(xparts_new, s->xparts, sizeof(struct xpart) * s->nr_parts);
  free(s->parts);
  free(s->xparts);
  s->parts = parts_new;
  s->xparts = xparts_new;

  /* Re-link the gparts. */
  if (s->nr_parts > 0 && s->nr_gparts > 0)
    part_relink_gparts(s->parts, s->nr_parts, 0);

  /* Re-allocate the local gparts. */
  if (e->verbose)
    message("Re-allocating gparts array from %zu to %zu.", s->size_gparts,
            (size_t)(s->nr_gparts * 1.2));
  s->size_gparts = s->nr_gparts * 1.2;
  struct gpart *gparts_new = NULL;
  if (posix_memalign((void **)&gparts_new, gpart_align,
                     sizeof(struct gpart) * s->size_gparts) != 0)
    error("Failed to allocate new gpart data.");
  memcpy(gparts_new, s->gparts, sizeof(struct gpart) * s->nr_gparts);
  free(s->gparts);
  s->gparts = gparts_new;

  /* Re-link the parts. */
  if (s->nr_parts > 0 && s->nr_gparts > 0)
    part_relink_parts(s->gparts, s->nr_gparts, s->parts);

#ifdef SWIFT_DEBUG_CHECKS

  /* Verify that the links are correct */
  for (size_t k = 0; k < s->nr_gparts; ++k) {

    if (s->gparts[k].id_or_neg_offset <= 0) {

      struct part *part = &s->parts[-s->gparts[k].id_or_neg_offset];

      if (part->gpart != &s->gparts[k]) error("Linking problem !");

      if (s->gparts[k].x[0] != part->x[0] || s->gparts[k].x[1] != part->x[1] ||
          s->gparts[k].x[2] != part->x[2])
        error("Linked particles are not at the same position !");
    }
  }
  for (size_t k = 0; k < s->nr_parts; ++k) {

    if (s->parts[k].gpart != NULL &&
        s->parts[k].gpart->id_or_neg_offset != -(ptrdiff_t)k)
      error("Linking problem !");
  }

#endif

#else
  error("SWIFT was not compiled with MPI support.");
#endif
}

/**
 * @brief Writes a snapshot with the current state of the engine
 *
 * @param e The #engine.
 */
void engine_dump_snapshot(struct engine *e) {

  struct clocks_time time1, time2;
  clocks_gettime(&time1);

  if (e->verbose) message("writing snapshot at t=%e.", e->time);

/* Dump... */
#if defined(WITH_MPI)
#if defined(HAVE_PARALLEL_HDF5)
  write_output_parallel(e, e->snapshotBaseName, e->internalUnits,
                        e->snapshotUnits, e->nodeID, e->nr_nodes,
                        MPI_COMM_WORLD, MPI_INFO_NULL);
#else
  write_output_serial(e, e->snapshotBaseName, e->internalUnits,
                      e->snapshotUnits, e->nodeID, e->nr_nodes, MPI_COMM_WORLD,
                      MPI_INFO_NULL);
#endif
#else
  write_output_single(e, e->snapshotBaseName, e->internalUnits,
                      e->snapshotUnits);
#endif

  clocks_gettime(&time2);
  if (e->verbose)
    message("writing particle properties took %.3f %s.",
            (float)clocks_diff(&time1, &time2), clocks_getunit());
}

#ifdef HAVE_SETAFFINITY
/**
 * @brief Returns the initial affinity the main thread is using.
 */
static cpu_set_t *engine_entry_affinity() {

  static int use_entry_affinity = 0;
  static cpu_set_t entry_affinity;

  if (!use_entry_affinity) {
    pthread_t engine = pthread_self();
    pthread_getaffinity_np(engine, sizeof(entry_affinity), &entry_affinity);
    use_entry_affinity = 1;
  }

  return &entry_affinity;
}
#endif

/**
 * @brief  Ensure the NUMA node on which we initialise (first touch) everything
 *  doesn't change before engine_init allocates NUMA-local workers.
 */
void engine_pin() {

#ifdef HAVE_SETAFFINITY
  cpu_set_t *entry_affinity = engine_entry_affinity();
  int pin;
  for (pin = 0; pin < CPU_SETSIZE && !CPU_ISSET(pin, entry_affinity); ++pin)
    ;

  cpu_set_t affinity;
  CPU_ZERO(&affinity);
  CPU_SET(pin, &affinity);
  if (sched_setaffinity(0, sizeof(affinity), &affinity) != 0) {
    error("failed to set engine's affinity");
  }
#else
  error("SWIFT was not compiled with support for pinning.");
#endif
}

/**
 * @brief Unpins the main thread.
 */
void engine_unpin() {
#ifdef HAVE_SETAFFINITY
  pthread_t main_thread = pthread_self();
  cpu_set_t *entry_affinity = engine_entry_affinity();
  pthread_setaffinity_np(main_thread, sizeof(*entry_affinity), entry_affinity);
#else
  error("SWIFT was not compiled with support for pinning.");
#endif
}

/**
 * @brief init an engine with the given number of threads, queues, and
 *      the given policy.
 *
 * @param e The #engine.
 * @param s The #space in which this #runner will run.
 * @param params The parsed parameter file.
 * @param nr_nodes The number of MPI ranks.
 * @param nodeID The MPI rank of this node.
 * @param nr_threads The number of threads per MPI rank.
 * @param with_aff use processor affinity, if supported.
 * @param policy The queuing policy to use.
 * @param verbose Is this #engine talkative ?
 * @param reparttype Type of repartitioning to use, if required.
 * @param internal_units The system of units used internally.
 * @param physical_constants The #phys_const used for this run.
 * @param hydro The #hydro_props used for this run.
 * @param potential The properties of the external potential.
 * @param cooling_func The properties of the cooling function.
 */
void engine_init(struct engine *e, struct space *s,
                 const struct swift_params *params, int nr_nodes, int nodeID,
                 int nr_threads, int with_aff, int policy, int verbose,
                 enum repartition_type reparttype,
                 const struct UnitSystem *internal_units,
                 const struct phys_const *physical_constants,
                 const struct hydro_props *hydro,
                 const struct external_potential *potential,
                 const struct cooling_function_data *cooling_func) {

  /* Clean-up everything */
  bzero(e, sizeof(struct engine));

  /* Store the values. */
  e->s = s;
  e->nr_threads = nr_threads;
  e->policy = policy;
  e->step = 0;
  e->nr_nodes = nr_nodes;
  e->nodeID = nodeID;
  e->proxy_ind = NULL;
  e->nr_proxies = 0;
  e->forcerebuild = 1;
  e->forcerepart = 0;
  e->reparttype = reparttype;
  e->links = NULL;
  e->nr_links = 0;
  e->timeBegin = parser_get_param_double(params, "TimeIntegration:time_begin");
  e->timeEnd = parser_get_param_double(params, "TimeIntegration:time_end");
  e->timeOld = e->timeBegin;
  e->time = e->timeBegin;
  e->ti_old = 0;
  e->ti_current = 0;
  e->timeStep = 0.;
  e->timeBase = 0.;
  e->timeBase_inv = 0.;
  e->drift_all = (policy & engine_policy_drift_all);
  e->internalUnits = internal_units;
  e->timeFirstSnapshot =
      parser_get_param_double(params, "Snapshots:time_first");
  e->deltaTimeSnapshot =
      parser_get_param_double(params, "Snapshots:delta_time");
  e->ti_nextSnapshot = 0;
  parser_get_param_string(params, "Snapshots:basename", e->snapshotBaseName);
  e->snapshotCompression =
      parser_get_opt_param_int(params, "Snapshots:compression", 0);
  e->snapshotUnits = malloc(sizeof(struct UnitSystem));
  units_init_default(e->snapshotUnits, params, "Snapshots", internal_units);
  e->dt_min = parser_get_param_double(params, "TimeIntegration:dt_min");
  e->dt_max = parser_get_param_double(params, "TimeIntegration:dt_max");
  e->file_stats = NULL;
  e->file_timesteps = NULL;
  e->deltaTimeStatistics =
      parser_get_param_double(params, "Statistics:delta_time");
  e->timeLastStatistics = e->timeBegin - e->deltaTimeStatistics;
  e->verbose = verbose;
  e->count_step = 0;
  e->wallclock_time = 0.f;
  e->physical_constants = physical_constants;
  e->hydro_properties = hydro;
  e->external_potential = potential;
  e->cooling_func = cooling_func;
  e->parameter_file = params;
  engine_rank = nodeID;

  /* Make the space link back to the engine. */
  s->e = e;

  /* Get the number of queues */
  int nr_queues =
      parser_get_opt_param_int(params, "Scheduler:nr_queues", nr_threads);
  if (nr_queues <= 0) nr_queues = e->nr_threads;
  if (nr_queues != nr_threads)
    message("Number of task queues set to %d", nr_queues);
  s->nr_queues = nr_queues;

/* Deal with affinity. For now, just figure out the number of cores. */
#if defined(HAVE_SETAFFINITY)
  const int nr_cores = sysconf(_SC_NPROCESSORS_ONLN);
  cpu_set_t *entry_affinity = engine_entry_affinity();
  const int nr_affinity_cores = CPU_COUNT(entry_affinity);

  if (nr_cores > CPU_SETSIZE) /* Unlikely, except on e.g. SGI UV. */
    error("must allocate dynamic cpu_set_t (too many cores per node)");

  char *buf = malloc((nr_cores + 1) * sizeof(char));
  buf[nr_cores] = '\0';
  for (int j = 0; j < nr_cores; ++j) {
    /* Reversed bit order from convention, but same as e.g. Intel MPI's
     * I_MPI_PIN_DOMAIN explicit mask: left-to-right, LSB-to-MSB. */
    buf[j] = CPU_ISSET(j, entry_affinity) ? '1' : '0';
  }

  if (verbose && with_aff) message("Affinity at entry: %s", buf);

  int *cpuid = NULL;
  cpu_set_t cpuset;

  if (with_aff) {

    cpuid = malloc(nr_affinity_cores * sizeof(int));

    int skip = 0;
    for (int k = 0; k < nr_affinity_cores; k++) {
      int c;
      for (c = skip; c < CPU_SETSIZE && !CPU_ISSET(c, entry_affinity); ++c)
        ;
      cpuid[k] = c;
      skip = c + 1;
    }

#if defined(HAVE_LIBNUMA) && defined(_GNU_SOURCE)
    if ((policy & engine_policy_cputight) != engine_policy_cputight) {

      if (numa_available() >= 0) {
        if (nodeID == 0) message("prefer NUMA-distant CPUs");

        /* Get list of numa nodes of all available cores. */
        int *nodes = malloc(nr_affinity_cores * sizeof(int));
        int nnodes = 0;
        for (int i = 0; i < nr_affinity_cores; i++) {
          nodes[i] = numa_node_of_cpu(cpuid[i]);
          if (nodes[i] > nnodes) nnodes = nodes[i];
        }
        nnodes += 1;

        /* Count cores per node. */
        int *core_counts = malloc(nnodes * sizeof(int));
        for (int i = 0; i < nr_affinity_cores; i++) {
          core_counts[nodes[i]] = 0;
        }
        for (int i = 0; i < nr_affinity_cores; i++) {
          core_counts[nodes[i]] += 1;
        }

        /* Index cores within each node. */
        int *core_indices = malloc(nr_affinity_cores * sizeof(int));
        for (int i = nr_affinity_cores - 1; i >= 0; i--) {
          core_indices[i] = core_counts[nodes[i]];
          core_counts[nodes[i]] -= 1;
        }

        /* Now sort so that we pick adjacent cpuids from different nodes
         * by sorting internal node core indices. */
        int done = 0;
        while (!done) {
          done = 1;
          for (int i = 1; i < nr_affinity_cores; i++) {
            if (core_indices[i] < core_indices[i - 1]) {
              int t = cpuid[i - 1];
              cpuid[i - 1] = cpuid[i];
              cpuid[i] = t;

              t = core_indices[i - 1];
              core_indices[i - 1] = core_indices[i];
              core_indices[i] = t;
              done = 0;
            }
          }
        }

        free(nodes);
        free(core_counts);
        free(core_indices);
      }
    }
#endif
  } else {
    if (nodeID == 0) message("no processor affinity used");

  } /* with_aff */

  /* Avoid (unexpected) interference between engine and runner threads. We can
   * do this once we've made at least one call to engine_entry_affinity and
   * maybe numa_node_of_cpu(sched_getcpu()), even if the engine isn't already
   * pinned. Also unpin this when asked to not pin at all (!with_aff). */
  engine_unpin();
#endif

  if (with_aff) {
#ifdef WITH_MPI
    printf("[%04i] %s engine_init: cpu map is [ ", nodeID,
           clocks_get_timesincestart());
#else
    printf("%s engine_init: cpu map is [ ", clocks_get_timesincestart());
#endif
    for (int i = 0; i < nr_affinity_cores; i++) printf("%i ", cpuid[i]);
    printf("].\n");
  }

  /* Are we doing stuff in parallel? */
  if (nr_nodes > 1) {
#ifndef WITH_MPI
    error("SWIFT was not compiled with MPI support.");
#else
    e->policy |= engine_policy_mpi;
    if ((e->proxies = (struct proxy *)malloc(sizeof(struct proxy) *
                                             engine_maxproxies)) == NULL)
      error("Failed to allocate memory for proxies.");
    bzero(e->proxies, sizeof(struct proxy) * engine_maxproxies);
    e->nr_proxies = 0;

    /* Initialise the repartitioning struct. */
    partition_repartition_init(&e->repartdata);

#endif
  }

  /* Open some files */
  if (e->nodeID == 0) {
    char energyfileName[200] = "";
    parser_get_opt_param_string(params, "Statistics:energy_file_name",
                                energyfileName,
                                engine_default_energy_file_name);
    sprintf(energyfileName + strlen(energyfileName), ".txt");
    e->file_stats = fopen(energyfileName, "w");
    fprintf(e->file_stats,
            "#%14s %14s %14s %14s %14s %14s %14s %14s %14s %14s %14s %14s %14s "
            "%14s\n",
            "Time", "Mass", "E_tot", "E_kin", "E_int", "E_pot", "E_radcool",
            "Entropy", "p_x", "p_y", "p_z", "ang_x", "ang_y", "ang_z");
    fflush(e->file_stats);

    char timestepsfileName[200] = "";
    parser_get_opt_param_string(params, "Statistics:timestep_file_name",
                                timestepsfileName,
                                engine_default_timesteps_file_name);

    sprintf(timestepsfileName + strlen(timestepsfileName), "_%d.txt",
            nr_nodes * nr_threads);
    e->file_timesteps = fopen(timestepsfileName, "w");
    fprintf(e->file_timesteps,
            "# Branch: %s\n# Revision: %s\n# Compiler: %s, Version: %s \n# "
            "Number of threads: %d\n# Number of MPI ranks: %d\n# Hydrodynamic "
            "scheme: %s\n# Hydrodynamic kernel: %s\n# No. of neighbours: %.2f "
            "+/- %.2f\n# Eta: %f\n",
            git_branch(), git_revision(), compiler_name(), compiler_version(),
            e->nr_threads, e->nr_nodes, SPH_IMPLEMENTATION, kernel_name,
            e->hydro_properties->target_neighbours,
            e->hydro_properties->delta_neighbours,
            e->hydro_properties->eta_neighbours);

    fprintf(e->file_timesteps, "# %6s %14s %14s %10s %10s %16s [%s]\n", "Step",
            "Time", "Time-step", "Updates", "g-Updates", "Wall-clock time",
            clocks_getunit());
    fflush(e->file_timesteps);
  }

  /* Print policy */
  engine_print_policy(e);

  /* Print information about the hydro scheme */
  if (e->policy & engine_policy_hydro)
    if (e->nodeID == 0) hydro_props_print(e->hydro_properties);

  /* Check we have sensible time bounds */
  if (e->timeBegin >= e->timeEnd)
    error(
        "Final simulation time (t_end = %e) must be larger than the start time "
        "(t_beg = %e)",
        e->timeEnd, e->timeBegin);

  /* Check we have sensible time-step values */
  if (e->dt_min > e->dt_max)
    error(
        "Minimal time-step size (%e) must be smaller than maximal time-step "
        "size (%e)",
        e->dt_min, e->dt_max);

  /* Deal with timestep */
  e->timeBase = (e->timeEnd - e->timeBegin) / max_nr_timesteps;
  e->timeBase_inv = 1.0 / e->timeBase;
  e->ti_current = 0;

  /* Fixed time-step case */
  if (e->policy & engine_policy_fixdt) {
    e->dt_min = e->dt_max;

    /* Find timestep on the timeline */
    int dti_timeline = max_nr_timesteps;
    while (e->dt_min < dti_timeline * e->timeBase) dti_timeline /= 2;

    e->dt_min = e->dt_max = dti_timeline * e->timeBase;

    if (e->nodeID == 0) message("Timestep set to %e", e->dt_max);
  } else {

    if (e->nodeID == 0) {
      message("Absolute minimal timestep size: %e", e->timeBase);

      float dt_min = e->timeEnd - e->timeBegin;
      while (dt_min > e->dt_min) dt_min /= 2.f;

      message("Minimal timestep size (on time-line): %e", dt_min);

      float dt_max = e->timeEnd - e->timeBegin;
      while (dt_max > e->dt_max) dt_max /= 2.f;

      message("Maximal timestep size (on time-line): %e", dt_max);
    }
  }

  if (e->dt_min < e->timeBase && e->nodeID == 0)
    error(
        "Minimal time-step size smaller than the absolute possible minimum "
        "dt=%e",
        e->timeBase);

  if (e->dt_max > (e->timeEnd - e->timeBegin) && e->nodeID == 0)
    error("Maximal time-step size larger than the simulation run time t=%e",
          e->timeEnd - e->timeBegin);

  /* Deal with outputs */
  if (e->deltaTimeSnapshot < 0.)
    error("Time between snapshots (%e) must be positive.",
          e->deltaTimeSnapshot);

  if (e->timeFirstSnapshot < e->timeBegin)
    error(
        "Time of first snapshot (%e) must be after the simulation start t=%e.",
        e->timeFirstSnapshot, e->timeBegin);

  /* Find the time of the first output */
  engine_compute_next_snapshot_time(e);

/* Construct types for MPI communications */
#ifdef WITH_MPI
  part_create_mpi_types();
#endif

  /* Initialize the threadpool. */
  threadpool_init(&e->threadpool, e->nr_threads);

  /* First of all, init the barrier and lock it. */
  if (pthread_mutex_init(&e->barrier_mutex, NULL) != 0)
    error("Failed to initialize barrier mutex.");
  if (pthread_cond_init(&e->barrier_cond, NULL) != 0)
    error("Failed to initialize barrier condition variable.");
  if (pthread_mutex_lock(&e->barrier_mutex) != 0)
    error("Failed to lock barrier mutex.");
  e->barrier_running = 0;
  e->barrier_launch = 0;
  e->barrier_launchcount = 0;

  /* Init the scheduler with enough tasks for the initial sorting tasks. */
  const int nr_tasks = 2 * s->tot_cells + 2 * e->nr_threads;
  scheduler_init(&e->sched, e->s, nr_tasks, nr_queues, scheduler_flag_steal,
                 e->nodeID, &e->threadpool);

  /* Allocate and init the threads. */
  if ((e->runners = (struct runner *)malloc(sizeof(struct runner) *
                                            e->nr_threads)) == NULL)
    error("Failed to allocate threads array.");
  for (int k = 0; k < e->nr_threads; k++) {
    e->runners[k].id = k;
    e->runners[k].e = e;
    e->barrier_running += 1;
    if (pthread_create(&e->runners[k].thread, NULL, &runner_main,
                       &e->runners[k]) != 0)
      error("Failed to create runner thread.");

    /* Try to pin the runner to a given core */
    if (with_aff &&
        (e->policy & engine_policy_setaffinity) == engine_policy_setaffinity) {
#if defined(HAVE_SETAFFINITY)

      /* Set a reasonable queue ID. */
      int coreid = k % nr_affinity_cores;
      e->runners[k].cpuid = cpuid[coreid];

      if (nr_queues < e->nr_threads)
        e->runners[k].qid = cpuid[coreid] * nr_queues / nr_affinity_cores;
      else
        e->runners[k].qid = k;

      /* Set the cpu mask to zero | e->id. */
      CPU_ZERO(&cpuset);
      CPU_SET(cpuid[coreid], &cpuset);

      /* Apply this mask to the runner's pthread. */
      if (pthread_setaffinity_np(e->runners[k].thread, sizeof(cpu_set_t),
                                 &cpuset) != 0)
        error("Failed to set thread affinity.");

#else
      error("SWIFT was not compiled with affinity enabled.");
#endif
    } else {
      e->runners[k].cpuid = k;
      e->runners[k].qid = k * nr_queues / e->nr_threads;
    }
    if (verbose) {
      if (with_aff)
        message("runner %i on cpuid=%i with qid=%i.", e->runners[k].id,
                e->runners[k].cpuid, e->runners[k].qid);
      else
        message("runner %i using qid=%i no cpuid.", e->runners[k].id,
                e->runners[k].qid);
    }
  }

/* Free the affinity stuff */
#if defined(HAVE_SETAFFINITY)
  if (with_aff) {
    free(cpuid);
  }
  free(buf);
#endif

  /* Wait for the runner threads to be in place. */
  while (e->barrier_running || e->barrier_launch)
    if (pthread_cond_wait(&e->barrier_cond, &e->barrier_mutex) != 0)
      error("Error while waiting for runner threads to get in place.");
}

/**
 * @brief Prints the current policy of an engine
 *
 * @param e The engine to print information about
 */
void engine_print_policy(struct engine *e) {

#ifdef WITH_MPI
  if (e->nodeID == 0) {
    printf("[0000] %s engine_policy: engine policies are [ ",
           clocks_get_timesincestart());
    for (int k = 1; k < 32; k++)
      if (e->policy & (1 << k)) printf(" %s ", engine_policy_names[k + 1]);
    printf(" ]\n");
    fflush(stdout);
  }
#else
  printf("%s engine_policy: engine policies are [ ",
         clocks_get_timesincestart());
  for (int k = 1; k < 32; k++)
    if (e->policy & (1 << k)) printf(" %s ", engine_policy_names[k + 1]);
  printf(" ]\n");
  fflush(stdout);
#endif
}

/**
 * @brief Computes the next time (on the time line) for a dump
 *
 * @param e The #engine.
 */
void engine_compute_next_snapshot_time(struct engine *e) {

  for (double time = e->timeFirstSnapshot;
       time < e->timeEnd + e->deltaTimeSnapshot; time += e->deltaTimeSnapshot) {

    /* Output time on the integer timeline */
    e->ti_nextSnapshot = (time - e->timeBegin) / e->timeBase;

    if (e->ti_nextSnapshot > e->ti_current) break;
  }

  /* Deal with last snapshot */
  if (e->ti_nextSnapshot >= max_nr_timesteps) {
    e->ti_nextSnapshot = -1;
    if (e->verbose) message("No further output time.");
  } else {

    /* Be nice, talk... */
    const float next_snapshot_time =
        e->ti_nextSnapshot * e->timeBase + e->timeBegin;
    if (e->verbose)
      message("Next output time set to t=%e.", next_snapshot_time);
  }
}

/**
 * @brief Frees up the memory allocated for this #engine
 */
void engine_clean(struct engine *e) {

  free(e->snapshotUnits);
  free(e->links);
  scheduler_clean(&e->sched);
  space_clean(e->s);
  threadpool_clean(&e->threadpool);
}
