/*******************************************************************************
 * This file is part of SWIFT.
 * Copyright (c) 2016 Peter W. Draper (p.w.draper@durham.ac.uk)
 *                    Pedro Gonnet (pedro.gonnet@durham.ac.uk)
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

/**
 *  @file partition.c
 *  @brief file of various techniques for partitioning and repartitioning
 *  a grid of cells into geometrically connected regions and distributing
 *  these around a number of MPI nodes.
 *
 *  Currently supported partitioning types: grid, vectorise and METIS.
 */

/* Config parameters. */
#include "../config.h"

/* Standard headers. */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

/* MPI headers. */
#ifdef WITH_MPI
#include <mpi.h>
/* METIS headers only used when MPI is also available. */
#ifdef HAVE_METIS
#include <metis.h>
#endif
#endif

/* Local headers. */
#include "const.h"
#include "debug.h"
#include "error.h"
#include "partition.h"
#include "space.h"
#include "tools.h"

/* Maximum weight used for METIS. */
#define metis_maxweight 10000.0f

/* Simple descriptions of initial partition types for reports. */
const char *initial_partition_name[] = {
    "gridded cells", "vectorized point associated cells",
    "METIS particle weighted cells", "METIS unweighted cells"};

/* Simple descriptions of repartition types for reports. */
const char *repartition_name[] = {
    "no", "METIS edge and vertex time weighted cells",
    "METIS particle count vertex weighted cells",
    "METIS time edge weighted cells",
    "METIS particle count vertex and time edge cells"};

/* Local functions, if needed. */
static int check_complete(struct space *s, int verbose, int nregions);

/*  Vectorisation support */
/*  ===================== */

#if defined(WITH_MPI)
/**
 *  @brief Pick a number of cell positions from a vectorised list.
 *
 *  Vectorise the cell space and pick positions in it for the number of
 *  expected regions using a single step. Vectorisation is guaranteed
 *  to work, providing there are more cells than regions.
 *
 *  @param s the space.
 *  @param nregions the number of regions
 *  @param samplecells the list of sample cell positions, size of 3*nregions
 */
static void pick_vector(struct space *s, int nregions, int *samplecells) {

  /* Get length of space and divide up. */
  int length = s->cdim[0] * s->cdim[1] * s->cdim[2];
  if (nregions > length) {
    error("Too few cells (%d) for this number of regions (%d)", length,
          nregions);
  }

  int step = length / nregions;
  int n = 0;
  int m = 0;
  int l = 0;

  for (int i = 0; i < s->cdim[0]; i++) {
    for (int j = 0; j < s->cdim[1]; j++) {
      for (int k = 0; k < s->cdim[2]; k++) {
        if (n == 0 && l < nregions) {
          samplecells[m++] = i;
          samplecells[m++] = j;
          samplecells[m++] = k;
          l++;
        }
        n++;
        if (n == step) n = 0;
      }
    }
  }
}
#endif

#if defined(WITH_MPI)
/**
 * @brief Partition the space.
 *
 * Using the sample positions as seeds pick cells that are geometrically
 * closest and apply the partition to the space.
 */
static void split_vector(struct space *s, int nregions, int *samplecells) {
  int n = 0;
  for (int i = 0; i < s->cdim[0]; i++) {
    for (int j = 0; j < s->cdim[1]; j++) {
      for (int k = 0; k < s->cdim[2]; k++) {
        int select = -1;
        float rsqmax = FLT_MAX;
        int m = 0;
        for (int l = 0; l < nregions; l++) {
          float dx = samplecells[m++] - i;
          float dy = samplecells[m++] - j;
          float dz = samplecells[m++] - k;
          float rsq = (dx * dx + dy * dy + dz * dz);
          if (rsq < rsqmax) {
            rsqmax = rsq;
            select = l;
          }
        }
        s->cells_top[n++].nodeID = select;
      }
    }
  }
}
#endif

/* METIS support
 * =============
 *
 * METIS partitions using a multi-level k-way scheme. We support using this in
 * a unweighted scheme, which works well and seems to be guaranteed, and a
 * weighted by the number of particles scheme. Note METIS is optional.
 *
 * Repartitioning is based on METIS and uses weights determined from the times
 * that cell tasks have taken. These weight the graph edges and vertices, or
 * just the edges, with vertex weights from the particle counts or none.
 */

#if defined(WITH_MPI) && defined(HAVE_METIS)
/**
 * @brief Fill the METIS xadj and adjncy arrays defining the graph of cells
 *        in a space.
 *
 * See the METIS manual if you want to understand this format. The cell graph
 * consists of all nodes as vertices with edges as the connections to all
 * neighbours, so we have 26 per vertex.
 *
 * @param s the space of cells.
 * @param adjncy the METIS adjncy array to fill, must be of size
 *               26 * the number of cells in the space.
 * @param xadj the METIS xadj array to fill, must be of size
 *             number of cells in space + 1. NULL for not used.
 */
static void graph_init_metis(struct space *s, idx_t *adjncy, idx_t *xadj) {

  /* Loop over all cells in the space. */
  int cid = 0;
  for (int l = 0; l < s->cdim[0]; l++) {
    for (int m = 0; m < s->cdim[1]; m++) {
      for (int n = 0; n < s->cdim[2]; n++) {

        /* Visit all neighbours of this cell, wrapping space at edges. */
        int p = 0;
        for (int i = -1; i <= 1; i++) {
          int ii = l + i;
          if (ii < 0)
            ii += s->cdim[0];
          else if (ii >= s->cdim[0])
            ii -= s->cdim[0];
          for (int j = -1; j <= 1; j++) {
            int jj = m + j;
            if (jj < 0)
              jj += s->cdim[1];
            else if (jj >= s->cdim[1])
              jj -= s->cdim[1];
            for (int k = -1; k <= 1; k++) {
              int kk = n + k;
              if (kk < 0)
                kk += s->cdim[2];
              else if (kk >= s->cdim[2])
                kk -= s->cdim[2];

              /* If not self, record id of neighbour. */
              if (i || j || k) {
                adjncy[cid * 26 + p] = cell_getid(s->cdim, ii, jj, kk);
                p++;
              }
            }
          }
        }

        /* Next cell. */
        cid++;
      }
    }
  }

  /* If given set xadj. */
  if (xadj != NULL) {
    xadj[0] = 0;
    for (int k = 0; k < s->nr_cells; k++) xadj[k + 1] = xadj[k] + 26;
  }
}
#endif

#if defined(WITH_MPI) && defined(HAVE_METIS)
/**
 * @brief Accumulate the counts of particles per cell.
 *
 * @param s the space containing the cells.
 * @param counts the number of particles per cell. Should be
 *               allocated as size s->nr_parts.
 */
static void accumulate_counts(struct space *s, float *counts) {

  struct part *parts = s->parts;
  int *cdim = s->cdim;
  double iwidth[3] = {s->iwidth[0], s->iwidth[1], s->iwidth[2]};
  double dim[3] = {s->dim[0], s->dim[1], s->dim[2]};

  for (size_t k = 0; k < s->nr_parts; k++) {
    for (int j = 0; j < 3; j++) {
      if (parts[k].x[j] < 0.0)
        parts[k].x[j] += dim[j];
      else if (parts[k].x[j] >= dim[j])
        parts[k].x[j] -= dim[j];
    }
    const int cid =
        cell_getid(cdim, parts[k].x[0] * iwidth[0], parts[k].x[1] * iwidth[1],
                   parts[k].x[2] * iwidth[2]);
    counts[cid]++;
  }
}
#endif

#if defined(WITH_MPI) && defined(HAVE_METIS)
/**
 * @brief Apply METIS cell list partitioning to a cell structure.
 *
 * Uses the results of part_metis_pick to assign each cell's nodeID to the
 * picked region index, thus partitioning the space into regions.
 *
 * @param s the space containing the cells to split into regions.
 * @param nregions number of regions.
 * @param celllist list of regions for each cell.
 */
static void split_metis(struct space *s, int nregions, int *celllist) {

  for (int i = 0; i < s->nr_cells; i++) s->cells_top[i].nodeID = celllist[i];
}
#endif

#if defined(WITH_MPI) && defined(HAVE_METIS)
/**
 * @brief Partition the given space into a number of connected regions.
 *
 * Split the space using METIS to derive a partitions using the
 * given edge and vertex weights. If no weights are given then an
 * unweighted partition is performed.
 *
 * @param s the space of cells to partition.
 * @param nregions the number of regions required in the partition.
 * @param vertexw weights for the cells, sizeof number of cells if used,
 *        NULL for unit weights.
 * @param edgew weights for the graph edges between all cells, sizeof number
 *        of cells * 26 if used, NULL for unit weights. Need to be packed
 *        in CSR format, so same as adjncy array.
 * @param celllist on exit this contains the ids of the selected regions,
 *        sizeof number of cells.
 */
static void pick_metis(struct space *s, int nregions, float *vertexw,
                       float *edgew, int *celllist) {

  /* Total number of cells. */
  int ncells = s->cdim[0] * s->cdim[1] * s->cdim[2];

  /* Nothing much to do if only using a single partition. Also avoids METIS
   * bug that doesn't handle this case well. */
  if (nregions == 1) {
    for (int i = 0; i < ncells; i++) celllist[i] = 0;
    return;
  }

  /* Allocate weights and adjacency arrays . */
  idx_t *xadj;
  if ((xadj = (idx_t *)malloc(sizeof(idx_t) * (ncells + 1))) == NULL)
    error("Failed to allocate xadj buffer.");
  idx_t *adjncy;
  if ((adjncy = (idx_t *)malloc(sizeof(idx_t) * 26 * ncells)) == NULL)
    error("Failed to allocate adjncy array.");
  idx_t *weights_v = NULL;
  if (vertexw != NULL)
    if ((weights_v = (idx_t *)malloc(sizeof(idx_t) * ncells)) == NULL)
      error("Failed to allocate vertex weights array");
  idx_t *weights_e = NULL;
  if (edgew != NULL)
    if ((weights_e = (idx_t *)malloc(26 * sizeof(idx_t) * ncells)) == NULL)
      error("Failed to allocate edge weights array");
  idx_t *regionid;
  if ((regionid = (idx_t *)malloc(sizeof(idx_t) * ncells)) == NULL)
    error("Failed to allocate regionid array");

  /* Define the cell graph. */
  graph_init_metis(s, adjncy, xadj);

  /* Init the vertex weights array. */
  if (vertexw != NULL) {
    for (int k = 0; k < ncells; k++) {
      if (vertexw[k] > 0) {
        weights_v[k] = vertexw[k];
      } else {
        weights_v[k] = 1;
      }
    }
  }

  /* Init the edges weights array. */
  if (edgew != NULL) {
    for (int k = 0; k < 26 * ncells; k++) {
      if (edgew[k] > 0) {
        weights_e[k] = edgew[k];
      } else {
        weights_e[k] = 1;
      }
    }
  }

  /* Set the METIS options. */
  idx_t options[METIS_NOPTIONS];
  METIS_SetDefaultOptions(options);
  options[METIS_OPTION_OBJTYPE] = METIS_OBJTYPE_CUT;
  options[METIS_OPTION_NUMBERING] = 0;
  options[METIS_OPTION_CONTIG] = 1;
  options[METIS_OPTION_NCUTS] = 10;
  options[METIS_OPTION_NITER] = 20;

  /* Call METIS. */
  idx_t one = 1;
  idx_t idx_ncells = ncells;
  idx_t idx_nregions = nregions;
  idx_t objval;

  /* Dump graph in METIS format */
  /* dumpMETISGraph("metis_graph", idx_ncells, one, xadj, adjncy,
   *                weights_v, weights_e, NULL);
   */

  if (METIS_PartGraphKway(&idx_ncells, &one, xadj, adjncy, weights_v, weights_e,
                          NULL, &idx_nregions, NULL, NULL, options, &objval,
                          regionid) != METIS_OK)
    error("Call to METIS_PartGraphKway failed.");

  /* Check that the regionids are ok. */
  for (int k = 0; k < ncells; k++)
    if (regionid[k] < 0 || regionid[k] >= nregions)
      error("Got bad nodeID %" PRIDX " for cell %i.", regionid[k], k);

  /* Set the cell list to the region index. */
  for (int k = 0; k < ncells; k++) {
    celllist[k] = regionid[k];
  }

  /* Clean up. */
  if (weights_v != NULL) free(weights_v);
  if (weights_e != NULL) free(weights_e);
  free(xadj);
  free(adjncy);
  free(regionid);
}
#endif

#if defined(WITH_MPI) && defined(HAVE_METIS)
/**
 * @brief Accumulate weights using the given tasks timings.
 *
 * @param partweights whether particle counts will be used as vertex weights.
 * @param s the space of cells holding our local particles.
 * @param tasks the completed tasks from the last engine step for our node.
 * @param nr_tasks the number of tasks.
 * @param repartdata the accumulated task timings data, reused if already
 *                   in use.
 */
static void repart_edge_metis_accumulate(int partweights, struct task *tasks,
                                         int nr_tasks,
                                         struct repartition_data *repartdata) {

  /* Create weight arrays using task ticks for vertices and edges (edges
   * assume the same graph structure as used in the part_ calls). */
  struct cell *cells = repartdata->s->cells_top;
  int nr_cells = repartdata->s->nr_cells;
  float wscale = 1e-3f;
  float wtot = 0.0f;

  /* Initialise the repartdata struct, if not already done so. */
  float *weights_v = repartdata->weights_v;
  float *weights_e = repartdata->weights_e;
  if (repartdata->nr_cells == 0) {

    /* Allocate and init weights. */
    if (repartdata->bothweights) {
      if ((weights_v = (float *)malloc(sizeof(float) * nr_cells)) == NULL)
        error("Failed to allocate vertex weights arrays.");
      for (int i = 0; i < nr_cells; i++)
        weights_v[i] = 0.0f;
    }
    if ((weights_e = (float *)malloc(sizeof(float) * 26 * nr_cells)) == NULL)
      error("Failed to allocate edge weights arrays.");
    for (int i = 0; i < 26 * nr_cells; i++)
      weights_e[i] = 0.0f;

    repartdata->weights_v = weights_v;
    repartdata->weights_e = weights_e;
    repartdata->nr_cells = nr_cells;
    repartdata->count = 0;
  }

  /* Sanity check and increment number of times this struct has been used. */
  if (repartdata->nr_cells != nr_cells)
    error("Repartition data cells counts do not match (%d != %d)",
          repartdata->nr_cells, nr_cells);
  repartdata->count++;

  /* Allocate and fill the adjncy indexing array defining the graph of
   * cells. */
  idx_t *inds;
  if ((inds = (idx_t *)malloc(sizeof(idx_t) * 26 * nr_cells)) == NULL)
    error("Failed to allocate the inds array");
  graph_init_metis(repartdata->s, inds, NULL);

  /* Generate task weights for vertices. */
  int taskvweights = (repartdata->bothweights && !partweights);

  /* Loop over the tasks... */
  for (int j = 0; j < nr_tasks; j++) {
    /* Get a pointer to the kth task. */
    struct task *t = &tasks[j];

    /* Skip un-interesting tasks. */
    if (t->type != task_type_self && t->type != task_type_pair &&
        t->type != task_type_sub_self && t->type != task_type_sub_self &&
        t->type != task_type_ghost && t->type != task_type_kick &&
        t->type != task_type_init)
      continue;

    /* Get the task weight. This can be slightly negative on multiple board
     * computers when the runners are not pinned to cores, don't stress just
     * make a report and ignore these tasks. */
    double w = (t->toc - t->tic) * wscale;
    if (w < 0.0) {
      message("Task toc before tic: -%.3f %s, (try using processor affinity).",
              clocks_from_ticks(t->tic - t->toc), clocks_getunit());
      w = 0.0;
    }
    wtot += w;

    /* Get the top-level cells involved. */
    struct cell *ci, *cj;
    for (ci = t->ci; ci->parent != NULL; ci = ci->parent)
      ;
    if (t->cj != NULL)
      for (cj = t->cj; cj->parent != NULL; cj = cj->parent)
        ;
    else
      cj = NULL;

    /* Get the cell IDs. */
    int cid = ci - cells;

    /* Different weights for different tasks. */
    if (t->type == task_type_ghost || t->type == task_type_kick) {
      /* Particle updates add only to vertex weight. */
      if (taskvweights) weights_v[cid] += w;

    }

    /* Self interaction? */
    else if ((t->type == task_type_self && ci->nodeID == repartdata->nodeID) ||
             (t->type == task_type_sub_self && cj == NULL &&
              ci->nodeID == repartdata->nodeID)) {
      /* Self interactions add only to vertex weight. */
      if (taskvweights) weights_v[cid] += w;

    }

    /* Pair? */
    else if (t->type == task_type_pair || (t->type == task_type_sub_pair)) {
      /* In-cell pair? */
      if (ci == cj) {
        /* Add weight to vertex for ci. */
        if (taskvweights) weights_v[cid] += w;

      }

      /* Distinct cells with local ci? */
      else if (ci->nodeID == repartdata->nodeID) {
        /* Index of the jth cell. */
        int cjd = cj - cells;

        /* Add half of weight to each cell. */
        if (taskvweights) {
          if (ci->nodeID == repartdata->nodeID) weights_v[cid] += 0.5 * w;
          if (cj->nodeID == repartdata->nodeID) weights_v[cjd] += 0.5 * w;
        }

        /* Add weights to edge with cjd. */
        int kk;
        for (kk = 26 * cid; inds[kk] != cjd; kk++);
        weights_e[kk] += w;

        /* Add weights to edge with cid. */
        for (kk = 26 * cjd; inds[kk] != cid; kk++);
        weights_e[kk] += w;
      }
    }
  }

  /* Re-calculate the vertices if using particle counts. */
  if (partweights && repartdata->bothweights) {
    accumulate_counts(repartdata->s, weights_v);

    /*  Rescale to balance times. */
    float vwscale = (float)wtot / (float)nr_tasks;
    for (int k = 0; k < nr_cells; k++) {
      weights_v[k] *= vwscale;
    }
  }
}
#endif

#if defined(WITH_MPI) && defined(HAVE_METIS)
/**
 * @brief Repartition the cells amongst the nodes using the given
 *        repartition data.
 *
 * @param repartdata the repartitioning weight data.
 */
static void repart_edge_metis(struct repartition_data *repartdata) {

  /* Merge the weights arrays across all nodes. */
  int res = 0;
  if (repartdata->bothweights) {
    if ((res = MPI_Reduce((repartdata->nodeID == 0) ? MPI_IN_PLACE : repartdata->weights_v,
                          repartdata->weights_v, repartdata->nr_cells,
                          MPI_FLOAT, MPI_SUM, 0, MPI_COMM_WORLD)) !=
        MPI_SUCCESS)
      mpi_error(res, "Failed to allreduce vertex weights.");
  }

  if ((res = MPI_Reduce((repartdata->nodeID == 0) ? MPI_IN_PLACE : repartdata->weights_e,
                        repartdata->weights_e, 26 * repartdata->nr_cells,
                        MPI_FLOAT, MPI_SUM, 0, MPI_COMM_WORLD)) !=
      MPI_SUCCESS)
    mpi_error(res, "Failed to allreduce edge weights.");

  /* Allocate cell list for the partition. */
  int *celllist = (int *)malloc(sizeof(int) * repartdata->nr_cells);
  if (celllist == NULL) error("Failed to allocate celllist");

  /* As of here, only one node needs to compute the partition. */
  if (repartdata->nodeID == 0) {
    /* Do rescale of all weights to avoid a large range. Large ranges have
     * been seen to cause an incomplete graph in METIS. */
    float wmin = FLT_MAX;
    float wmax = 0;
    for (int k = 0; k < 26 * repartdata->nr_cells; k++) {
      wmax = repartdata->weights_e[k] > wmax ? repartdata->weights_e[k] : wmax;
      wmin = repartdata->weights_e[k] < wmin ? repartdata->weights_e[k] : wmin;
    }
    if (repartdata->bothweights) {
      for (int k = 0; k < repartdata->nr_cells; k++) {
        wmax = repartdata->weights_v[k] > wmax ? repartdata->weights_v[k] : wmax;
        wmin = repartdata->weights_v[k] < wmin ? repartdata->weights_v[k] : wmin;
      }
    }

    if ((wmax - wmin) > metis_maxweight) {
      float wscale = metis_maxweight / (wmax - wmin);
      for (int k = 0; k < 26 * repartdata->nr_cells; k++) {
        repartdata->weights_e[k] = (repartdata->weights_e[k] - wmin) * wscale + 1;
      }
      if (repartdata->bothweights) {
        for (int k = 0; k < repartdata->nr_cells; k++) {
          repartdata->weights_v[k] = (repartdata->weights_v[k] - wmin) * wscale + 1;
        }
      }
    }

    /* Make sure there are no zero weights. */
    for (int k = 0; k < 26 * repartdata->nr_cells; k++)
      if (repartdata->weights_e[k] == 0.0f) repartdata->weights_e[k] = 1.0f;
    if (repartdata->bothweights)
      for (int k = 0; k < repartdata->nr_cells; k++)
        if ((repartdata->weights_v[k]) == 0) repartdata->weights_v[k] = 1.0f;

    /* And partition, use both weights or not as requested. */
    if (repartdata->bothweights) {
      pick_metis(repartdata->s, repartdata->nr_nodes, repartdata->weights_v,
                 repartdata->weights_e, celllist);
    } else {
      pick_metis(repartdata->s, repartdata->nr_nodes, NULL,
                 repartdata->weights_e, celllist);
    }

    /* Check that all cells have good values. */
    for (int k = 0; k < repartdata->nr_cells; k++)
      if (celllist[k] < 0 || celllist[k] >= repartdata->nr_nodes)
        error("Got bad nodeID %d for cell %i.", celllist[k], k);

    /* Check that the partition is complete and all nodes have some work. */
    int present[repartdata->nr_nodes];
    int failed = 0;
    for (int i = 0; i < repartdata->nr_nodes; i++) present[i] = 0;
    for (int i = 0; i < repartdata->nr_cells; i++) present[celllist[i]]++;
    for (int i = 0; i < repartdata->nr_nodes; i++) {
      if (!present[i]) {
        failed = 1;
        message("Node %d is not present after repartition", i);
      }
    }

    /* If partition failed continue with the current one, but make this
     * clear. */
    if (failed) {
      message(
          "WARNING: METIS repartition has failed, continuing with "
          "the current partition, load balance will not be optimal");
      for (int k = 0; k < repartdata->nr_cells; k++)
          celllist[k] = repartdata->s->cells_top[k].nodeID;
    }
  }

  /* Distribute the celllist partition and apply. */
  if ((res = MPI_Bcast(celllist, repartdata->nr_cells, MPI_INT, 0, MPI_COMM_WORLD)) !=
      MPI_SUCCESS)
    mpi_error(res, "Failed to bcast the cell list");

  /* And apply to our cells */
  split_metis(repartdata->s, repartdata->nr_nodes, celllist);

  /* Clean up. */
  free(celllist);
}
#endif

/**
 * @brief Repartition the cells amongst the nodes using vertex weights
 *
 * @param s The space containing the local cells.
 * @param nodeID our MPI node id.
 * @param nr_nodes number of MPI nodes.
 */
#if defined(WITH_MPI) && defined(HAVE_METIS)
static void repart_vertex_metis(struct space *s, int nodeID, int nr_nodes) {

  /* Use particle counts as vertex weights. */
  /* Space for particles per cell counts, which will be used as weights. */
  float *weights = NULL;
  if ((weights = (float *)malloc(sizeof(int) * s->nr_cells)) == NULL)
    error("Failed to allocate weights buffer.");

  /* Check each particle and accumulate the counts per cell. */
  accumulate_counts(s, weights);

  /* Get all the counts from all the nodes. */
  int res;
  if ((res = MPI_Allreduce(MPI_IN_PLACE, weights, s->nr_cells, MPI_FLOAT,
                           MPI_SUM, MPI_COMM_WORLD)) != MPI_SUCCESS)
    mpi_error(res, "Failed to allreduce particle cell weights.");

  /* Main node does the partition calculation. */
  int *celllist = (int *)malloc(sizeof(int) * s->nr_cells);
  if (celllist == NULL) error("Failed to allocate celllist");

  if (nodeID == 0) pick_metis(s, nr_nodes, weights, NULL, celllist);

  /* Distribute the celllist partition and apply. */
  if ((res = MPI_Bcast(celllist, s->nr_cells, MPI_INT, 0, MPI_COMM_WORLD)) !=
      MPI_SUCCESS)
    mpi_error(res, "Failed to bcast the cell list");

  /* And apply to our cells */
  split_metis(s, nr_nodes, celllist);

  free(weights);
  free(celllist);
}
#endif

/**
 * @brief Accumulate task based weights before performing a repartition.
 *
 * This function should be called before partition_repartition so that any
 * timed based weights can be calculated. If called with an existing structure
 * then new times will be added to those that already exist so that tasks
 * weights can be accumulated over a number of time steps.
 *
 * @param reparttype the type of repartition to attempt, see the repart_type
 *                   enum.
 * @param nodeID our nodeID.
 * @param nr_nodes the number of nodes.
 * @param s the space of cells holding our local particles.
 * @param tasks the completed tasks from the last engine step for our node.
 * @param nr_tasks the number of tasks.
 * @param repartdata the accumulated task timings data.
 */
void partition_repartition_accumulate(enum repartition_type reparttype,
                                      int nodeID, int nr_nodes,
                                      struct space *s, struct task *tasks,
                                      int nr_tasks,
                                      struct repartition_data *repartdata) {

#if defined(WITH_MPI) && defined(HAVE_METIS)

  /* Initialise the repart data struct for accumulation of weights. */
  if (repartdata->nr_cells == 0) {
    repartdata->s = s;
    repartdata->reparttype = reparttype;
    repartdata->nodeID = nodeID;
    repartdata->nr_nodes = nr_nodes;
  }
  else {
    if (repartdata->s != s ||
        repartdata->reparttype != reparttype ||
        repartdata->nodeID != nodeID ||
        repartdata->nr_nodes != nr_nodes)
      error("mismatch of fundamental data, cannot accumulate weights");
  }

  /* Only need to do actual accumulation for task time weighted schemes. */
  if (reparttype == REPART_METIS_BOTH || reparttype == REPART_METIS_EDGE ||
      reparttype == REPART_METIS_VERTEX_EDGE) {

    int partweights;
    if (reparttype == REPART_METIS_VERTEX_EDGE)
      partweights = 1;
    else
      partweights = 0;

    if (reparttype == REPART_METIS_BOTH)
      repartdata->bothweights = 1;
    else
      repartdata->bothweights = 0;

    repart_edge_metis_accumulate(partweights, tasks, nr_tasks, repartdata);
  }
#else
  error("SWIFT was not compiled with METIS support.");
#endif
}

/**
 * @brief Repartition the space based on repartitioning data.
 *
 * Note that at the end of this process all the cells will be re-distributed
 * across the nodes, but the particles themselves will not be.
 *
 * The repartion_data struct is created by (repeatably) calling
 * partition_repartition_accumulate, to accumulate the various tasks times
 * used in steps of the engine.
 *
 * @param repartdata processed by partition_repartition_accumulate.
 */
void partition_repartition(struct repartition_data *repartdata) {

#if defined(WITH_MPI) && defined(HAVE_METIS)

  if (repartdata->reparttype == REPART_METIS_BOTH ||
      repartdata->reparttype == REPART_METIS_EDGE ||
      repartdata->reparttype == REPART_METIS_VERTEX_EDGE) {

    repart_edge_metis(repartdata);

  } else if (repartdata->reparttype == REPART_METIS_VERTEX) {

      repart_vertex_metis(repartdata->s, repartdata->nodeID,
                          repartdata->nr_nodes);

  } else {
    error("Unknown repartition type");
  }
#else
  error("SWIFT was not compiled with METIS support.");
#endif
}

/**
 * @brief Initialise repartitioning data struct.
 *
 * @param repartdata repartition data struct to initialise.
 */
void partition_repartition_init(struct repartition_data *repartdata) {

  /* All counts to zero. */
  bzero(repartdata, sizeof(struct repartition_data));
}

/**
 * @brief Clear repartitioning data struct reinitialising and freeing any
 *        associated resources.
 *
 * @param repartdata repartition data struct to initialise.
 */
void partition_repartition_clear(struct repartition_data *repartdata) {

  /* Free any allocated memory. */
  if (repartdata->weights_e != NULL) {
    free(repartdata->weights_e);
  }
  if (repartdata->weights_v != NULL) {
    free(repartdata->weights_v);
  }
  partition_repartition_init(repartdata);
}

/**
 * @brief Initial partition of space cells.
 *
 * Cells are assigned to a node on the basis of various schemes, all of which
 * should attempt to distribute them in geometrically close regions to
 * minimise the movement of particles.
 *
 * Note that the partition type is a suggestion and will be ignored if that
 * scheme fails. In that case we fallback to a vectorised scheme, that is
 * guaranteed to work provided we have more cells than nodes.
 *
 * @param initial_partition the type of partitioning to try.
 * @param nodeID our nodeID.
 * @param nr_nodes the number of nodes.
 * @param s the space of cells.
 */
void partition_initial_partition(struct partition *initial_partition,
                                 int nodeID, int nr_nodes, struct space *s) {

  /* Geometric grid partitioning. */
  if (initial_partition->type == INITPART_GRID) {
    int j, k;
    int ind[3];
    struct cell *c;

    /* If we've got the wrong number of nodes, fail. */
    if (nr_nodes !=
        initial_partition->grid[0] * initial_partition->grid[1] *
            initial_partition->grid[2])
      error("Grid size does not match number of nodes.");

    /* Run through the cells and set their nodeID. */
    // message("s->dim = [%e,%e,%e]", s->dim[0], s->dim[1], s->dim[2]);
    for (k = 0; k < s->nr_cells; k++) {
      c = &s->cells_top[k];
      for (j = 0; j < 3; j++)
        ind[j] = c->loc[j] / s->dim[j] * initial_partition->grid[j];
      c->nodeID = ind[0] +
                  initial_partition->grid[0] *
                      (ind[1] + initial_partition->grid[1] * ind[2]);
      // message("cell at [%e,%e,%e]: ind = [%i,%i,%i], nodeID = %i", c->loc[0],
      // c->loc[1], c->loc[2], ind[0], ind[1], ind[2], c->nodeID);
    }

    /* The grid technique can fail, so check for this before proceeding. */
    if (!check_complete(s, (nodeID == 0), nr_nodes)) {
      if (nodeID == 0)
        message("Grid initial partition failed, using a vectorised partition");
      initial_partition->type = INITPART_VECTORIZE;
      partition_initial_partition(initial_partition, nodeID, nr_nodes, s);
      return;
    }

  } else if (initial_partition->type == INITPART_METIS_WEIGHT ||
             initial_partition->type == INITPART_METIS_NOWEIGHT) {
#if defined(WITH_MPI) && defined(HAVE_METIS)
    /* Simple k-way partition selected by METIS using cell particle counts as
     * weights or not. Should be best when starting with a inhomogeneous dist.
     */

    /* Space for particles per cell counts, which will be used as weights or
     * not. */
    float *weights = NULL;
    if (initial_partition->type == INITPART_METIS_WEIGHT) {
      if ((weights = (float *)malloc(sizeof(float) * s->nr_cells)) == NULL)
        error("Failed to allocate weights buffer.");
      bzero(weights, sizeof(float) * s->nr_cells);

      /* Check each particle and accumulate the counts per cell. */
      struct part *parts = s->parts;
      int *cdim = s->cdim;
      double iwidth[3], dim[3];
      iwidth[0] = s->iwidth[0];
      iwidth[1] = s->iwidth[1];
      iwidth[2] = s->iwidth[2];
      dim[0] = s->dim[0];
      dim[1] = s->dim[1];
      dim[2] = s->dim[2];
      for (size_t k = 0; k < s->nr_parts; k++) {
        for (int j = 0; j < 3; j++) {
          if (parts[k].x[j] < 0.0)
            parts[k].x[j] += dim[j];
          else if (parts[k].x[j] >= dim[j])
            parts[k].x[j] -= dim[j];
        }
        const int cid =
            cell_getid(cdim, parts[k].x[0] * iwidth[0],
                       parts[k].x[1] * iwidth[1], parts[k].x[2] * iwidth[2]);
        weights[cid]++;
      }

      /* Get all the counts from all the nodes. */
      if (MPI_Allreduce(MPI_IN_PLACE, weights, s->nr_cells, MPI_FLOAT, MPI_SUM,
                        MPI_COMM_WORLD) != MPI_SUCCESS)
        error("Failed to allreduce particle cell weights.");
    }

    /* Main node does the partition calculation. */
    int *celllist = (int *)malloc(sizeof(int) * s->nr_cells);
    if (celllist == NULL) error("Failed to allocate celllist");
    if (nodeID == 0) pick_metis(s, nr_nodes, weights, NULL, celllist);

    /* Distribute the celllist partition and apply. */
    int res = MPI_Bcast(celllist, s->nr_cells, MPI_INT, 0, MPI_COMM_WORLD);
    if (res != MPI_SUCCESS) mpi_error(res, "Failed to bcast the cell list");

    /* And apply to our cells */
    split_metis(s, nr_nodes, celllist);

    /* It's not known if this can fail, but check for this before
     * proceeding. */
    if (!check_complete(s, (nodeID == 0), nr_nodes)) {
      if (nodeID == 0)
        message("METIS initial partition failed, using a vectorised partition");
      initial_partition->type = INITPART_VECTORIZE;
      partition_initial_partition(initial_partition, nodeID, nr_nodes, s);
    }

    if (weights != NULL) free(weights);
    free(celllist);
#else
    error("SWIFT was not compiled with METIS support");
#endif

  } else if (initial_partition->type == INITPART_VECTORIZE) {

#if defined(WITH_MPI)
    /* Vectorised selection, guaranteed to work for samples less than the
     * number of cells, but not very clumpy in the selection of regions. */
    int *samplecells = (int *)malloc(sizeof(int) * nr_nodes * 3);
    if (samplecells == NULL) error("Failed to allocate samplecells");

    if (nodeID == 0) {
      pick_vector(s, nr_nodes, samplecells);
    }

    /* Share the samplecells around all the nodes. */
    int res = MPI_Bcast(samplecells, nr_nodes * 3, MPI_INT, 0, MPI_COMM_WORLD);
    if (res != MPI_SUCCESS)
      mpi_error(res, "Failed to bcast the partition sample cells.");

    /* And apply to our cells */
    split_vector(s, nr_nodes, samplecells);
    free(samplecells);
#else
    error("SWIFT was not compiled with MPI support");
#endif
  }
}

/**
 * @brief Initialises the partition and re-partition scheme from the parameter
 *        file
 *
 * @param partition The #partition scheme to initialise.
 * @param reparttype The repartition scheme to initialise.
 * @param params The parsed parameter file.
 * @param nr_nodes The number of MPI nodes we are running on.
 */
void partition_init(struct partition *partition,
                    enum repartition_type *reparttype,
                    const struct swift_params *params, int nr_nodes) {

#ifdef WITH_MPI

/* Defaults make use of METIS if available */
#ifdef HAVE_METIS
  char default_repart = 'b';
  ;
  char default_part = 'm';
#else
  char default_repart = 'n';
  char default_part = 'g';
#endif

  /* Set a default grid so that grid[0]*grid[1]*grid[2] == nr_nodes. */
  factor(nr_nodes, &partition->grid[0], &partition->grid[1]);
  factor(nr_nodes / partition->grid[1], &partition->grid[0],
         &partition->grid[2]);
  factor(partition->grid[0] * partition->grid[1], &partition->grid[1],
         &partition->grid[0]);

  /* Now let's check what the user wants as an initial domain. */
  const char part_type = parser_get_opt_param_char(
      params, "DomainDecomposition:initial_type", default_part);

  switch (part_type) {
    case 'g':
      partition->type = INITPART_GRID;
      break;
    case 'v':
      partition->type = INITPART_VECTORIZE;
      break;
#ifdef HAVE_METIS
    case 'm':
      partition->type = INITPART_METIS_NOWEIGHT;
      break;
    case 'w':
      partition->type = INITPART_METIS_WEIGHT;
      break;
    default:
      message("Invalid choice of initial partition type '%c'.", part_type);
      error("Permitted values are: 'g','m','v' or 'w'.");
#else
    default:
      message("Invalid choice of initial partition type '%c'.", part_type);
      error("Permitted values are: 'g' or 'v' when compiled without metis.");
#endif
  }

  /* In case of grid, read more parameters */
  if (part_type == 'g') {
    partition->grid[0] = parser_get_opt_param_int(
        params, "DomainDecomposition:initial_grid_x", partition->grid[0]);
    partition->grid[1] = parser_get_opt_param_int(
        params, "DomainDecomposition:initial_grid_y", partition->grid[1]);
    partition->grid[2] = parser_get_opt_param_int(
        params, "DomainDecomposition:initial_grid_z", partition->grid[2]);
  }

  /* Now let's check what the user wants as a repartition strategy */
  const char repart_type = parser_get_opt_param_char(
      params, "DomainDecomposition:repartition_type", default_repart);

  switch (repart_type) {
    case 'n':
      *reparttype = REPART_NONE;
      break;
#ifdef HAVE_METIS
    case 'b':
      *reparttype = REPART_METIS_BOTH;
      break;
    case 'e':
      *reparttype = REPART_METIS_EDGE;
      break;
    case 'v':
      *reparttype = REPART_METIS_VERTEX;
      break;
    case 'x':
      *reparttype = REPART_METIS_VERTEX_EDGE;
      break;
    default:
      message("Invalid choice of re-partition type '%c'.", repart_type);
      error("Permitted values are: 'b','e','n', 'v' or 'x'.");
#else
    default:
      message("Invalid choice of re-partition type '%c'.", repart_type);
      error("Permitted values are: 'n' when compiled without metis.");
#endif
  }

#else
  error("SWIFT was not compiled with MPI support");
#endif
}

/*  General support */
/*  =============== */

/**
 * @brief Check if all regions have been assigned a node in the
 *        cells of a space.
 *
 * @param s the space containing the cells to check.
 * @param nregions number of regions expected.
 * @param verbose if true report the missing regions.
 * @return true if all regions have been found, false otherwise.
 */
static int check_complete(struct space *s, int verbose, int nregions) {

  int *present = (int *)malloc(sizeof(int) * nregions);
  if (present == NULL) error("Failed to allocate present array");

  int failed = 0;
  for (int i = 0; i < nregions; i++) present[i] = 0;
  for (int i = 0; i < s->nr_cells; i++) {
    if (s->cells_top[i].nodeID <= nregions)
      present[s->cells_top[i].nodeID]++;
    else
      message("Bad nodeID: %d", s->cells_top[i].nodeID);
  }
  for (int i = 0; i < nregions; i++) {
    if (!present[i]) {
      failed = 1;
      if (verbose) message("Region %d is not present in partition", i);
    }
  }
  free(present);
  return (!failed);
}

/**
 * @brief Partition a space of cells based on another space of cells.
 *
 * The two spaces are expected to be at different cell sizes, so what we'd
 * like to do is assign the second space to geometrically closest nodes
 * of the first, with the effect of minimizing particle movement when
 * rebuilding the second space from the first.
 *
 * Since two spaces cannot exist simultaneously the old space is actually
 * required in a decomposed state. These are the old cells sizes and counts
 * per dimension, along with a list of the old nodeIDs. The old nodeIDs are
 * indexed by the cellid (see cell_getid()), so should be stored that way.
 *
 * On exit the new space cells will have their nodeIDs assigned.
 *
 * @param oldh the cell dimensions of old space.
 * @param oldcdim number of cells per dimension in old space.
 * @param oldnodeIDs the nodeIDs of cells in the old space, indexed by old
 *cellid.
 * @param s the space to be partitioned.
 *
 * @return 1 if the new space contains nodeIDs from all nodes, 0 otherwise.
 */
int partition_space_to_space(double *oldh, double *oldcdim, int *oldnodeIDs,
                             struct space *s) {

  /* Loop over all the new cells. */
  int nr_nodes = 0;
  for (int i = 0; i < s->cdim[0]; i++) {
    for (int j = 0; j < s->cdim[1]; j++) {
      for (int k = 0; k < s->cdim[2]; k++) {

        /* Scale indices to old cell space. */
        const int ii = rint(i * s->iwidth[0] * oldh[0]);
        const int jj = rint(j * s->iwidth[1] * oldh[1]);
        const int kk = rint(k * s->iwidth[2] * oldh[2]);

        const int cid = cell_getid(s->cdim, i, j, k);
        const int oldcid = cell_getid(oldcdim, ii, jj, kk);
        s->cells_top[cid].nodeID = oldnodeIDs[oldcid];

        if (oldnodeIDs[oldcid] > nr_nodes) nr_nodes = oldnodeIDs[oldcid];
      }
    }
  }

  /* Check we have all nodeIDs present in the resample. */
  return check_complete(s, 1, nr_nodes + 1);
}
