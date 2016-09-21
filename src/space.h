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
#ifndef SWIFT_SPACE_H
#define SWIFT_SPACE_H

/* Config parameters. */
#include "../config.h"

/* Some standard headers. */
#include <stddef.h>

/* Includes. */
#include "cell.h"
#include "lock.h"
#include "parser.h"
#include "part.h"
#include "space.h"

/* Some constants. */
#define space_cellallocchunk 1000
#define space_splitsize_default 400
#define space_maxsize_default 8000000
#define space_subsize_default 64000000
#define space_maxcount_default 10000
#define space_stretch 1.10f
#define space_maxreldx 0.25f

/* Maximum allowed depth of cell splits. */
#define space_cell_maxdepth 52

/* Split size. */
extern int space_splitsize;
extern int space_maxsize;
extern int space_subsize;
extern int space_maxcount;

/* Map shift vector to sortlist. */
extern const int sortlistID[27];

/**
 * @brief The space in which the cells and particles reside.
 */
struct space {

  /*! Spatial extent. */
  double dim[3];

  /*! Is the space periodic? */
  int periodic;

  /*! Are we doing gravity? */
  int gravity;

  /*! Width of the top-level cells. */
  double width[3];

  /*! Inverse of the top-level cell width */
  double iwidth[3];

  /*! The minimum top-level cell width allowed. */
  double cell_min;

  /*! Current maximum displacement for particles. */
  float dx_max;

  /*! Space dimensions in number of top-cells. */
  int cdim[3];

  /*! Maximal depth reached by the tree */
  int maxdepth;

  /*! Number of top-level cells. */
  int nr_cells;

  /*! Total number of cells (top- and sub-) */
  int tot_cells;

  /*! The (level 0) cells themselves. */
  struct cell *cells_top;

  /*! Buffer of unused cells for the sub-cells. */
  struct cell *cells_sub;

  /*! The total number of parts in the space. */
  size_t nr_parts, size_parts;

  /*! The total number of g-parts in the space. */
  size_t nr_gparts, size_gparts;

  /*! The particle data (cells have pointers to this). */
  struct part *parts;

  /*! The extended particle data (cells have pointers to this). */
  struct xpart *xparts;

  /*! The g-particle data (cells have pointers to this). */
  struct gpart *gparts;

  /*! General-purpose lock for this space. */
  swift_lock_type lock;

  /*! Number of queues in the system. */
  int nr_queues;

  /*! The associated engine. */
  struct engine *e;

#ifdef WITH_MPI

  /*! Buffers for parts that we will receive from foreign cells. */
  struct part *parts_foreign;
  size_t nr_parts_foreign, size_parts_foreign;

  /*! Buffers for g-parts that we will receive from foreign cells. */
  struct gpart *gparts_foreign;
  size_t nr_gparts_foreign, size_gparts_foreign;

#endif
};

/* function prototypes. */
void space_parts_sort(struct space *s, int *ind, size_t N, int min, int max,
                      int verbose);
void space_gparts_sort(struct space *s, int *ind, size_t N, int min, int max,
                       int verbose);
struct cell *space_getcell(struct space *s);
int space_getsid(struct space *s, struct cell **ci, struct cell **cj,
                 double *shift);
void space_init(struct space *s, const struct swift_params *params,
                double dim[3], struct part *parts, struct gpart *gparts,
                size_t Npart, size_t Ngpart, int periodic, int gravity,
                int verbose, int dry_run);
void space_sanitize(struct space *s);
void space_map_cells_pre(struct space *s, int full,
                         void (*fun)(struct cell *c, void *data), void *data);
void space_map_parts(struct space *s,
                     void (*fun)(struct part *p, struct cell *c, void *data),
                     void *data);
void space_map_parts_xparts(struct space *s,
                            void (*fun)(struct part *p, struct xpart *xp,
                                        struct cell *c));
void space_map_cells_post(struct space *s, int full,
                          void (*fun)(struct cell *c, void *data), void *data);
void space_parts_sort_mapper(void *map_data, int num_elements,
                             void *extra_data);
void space_gparts_sort_mapper(void *map_data, int num_elements,
                              void *extra_data);
void space_rebuild(struct space *s, double h_max, int verbose);
void space_recycle(struct space *s, struct cell *c);
void space_split(struct space *s, struct cell *cells, int nr_cells,
                 int verbose);
void space_split_mapper(void *map_data, int num_elements, void *extra_data);
void space_do_parts_sort();
void space_do_gparts_sort();
void space_init_parts(struct space *s);
void space_init_gparts(struct space *s);
void space_link_cleanup(struct space *s);
void space_clean(struct space *s);

#endif /* SWIFT_SPACE_H */
