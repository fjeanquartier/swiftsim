/*******************************************************************************
 * This file is part of SWIFT.
 * Copyright (c) 2012 Pedro Gonnet (pedro.gonnet@durham.ac.uk),
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
#include <errno.h>
#include <fenv.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* MPI headers. */
#ifdef WITH_MPI
#include <mpi.h>
#endif

/* Local headers. */
#include "swift.h"

/* Engine policy flags. */
#ifndef ENGINE_POLICY
#define ENGINE_POLICY engine_policy_none
#endif

/* Global profiler. */
struct profiler prof;

/**
 * @brief Help messages for the command line parameters.
 */
void print_help_message(void) {

  printf("\nUsage: swift [OPTION]... PARAMFILE\n");
  printf("       swift_mpi [OPTION]... PARAMFILE\n\n");

  printf("Valid options are:\n");
  printf("  %2s %14s %s\n", "-a", "", "Pin runners using processor affinity.");
  printf("  %2s %14s %s\n", "-c", "",
         "Run with cosmological time integration.");
  printf("  %2s %14s %s\n", "-C", "", "Run with cooling.");
  printf(
      "  %2s %14s %s\n", "-d", "",
      "Dry run. Read the parameter file, allocate memory but does not read ");
  printf(
      "  %2s %14s %s\n", "", "",
      "the particles from ICs and exit before the start of time integration.");
  printf("  %2s %14s %s\n", "", "",
         "Allows user to check validy of parameter and IC files as well as "
         "memory limits.");
  printf("  %2s %14s %s\n", "-D", "",
         "Always drift all particles even the ones far from active particles. "
         "This emulates");
  printf("  %2s %14s %s\n", "", "",
         "Gadget-[23] and GIZMO's default behaviours.");
  printf("  %2s %14s %s\n", "-e", "",
         "Enable floating-point exceptions (debugging mode).");
  printf("  %2s %14s %s\n", "-f", "{int}",
         "Overwrite the CPU frequency (Hz) to be used for time measurements.");
  printf("  %2s %14s %s\n", "-g", "",
         "Run with an external gravitational potential.");
  printf("  %2s %14s %s\n", "-G", "", "Run with self-gravity.");
  printf("  %2s %14s %s\n", "-M", "",
         "Reconstruct the multipoles every time-step.");
  printf("  %2s %14s %s\n", "-n", "{int}",
         "Execute a fixed number of time steps. When unset use the time_end "
         "parameter to stop.");
  printf("  %2s %14s %s\n", "-o", "{str}",
         "Generate a default output parameter file.");
  printf("  %2s %14s %s\n", "-P", "{sec:par:val}",
         "Set parameter value and overwrites values read from the parameters "
         "file. Can be used more than once.");
  printf("  %2s %14s %s\n", "-r", "", "Continue using restart files.");
  printf("  %2s %14s %s\n", "-s", "", "Run with hydrodynamics.");
  printf("  %2s %14s %s\n", "-S", "", "Run with stars.");
  printf("  %2s %14s %s\n", "-t", "{int}",
         "The number of threads to use on each MPI rank. Defaults to 1 if not "
         "specified.");
  printf("  %2s %14s %s\n", "-T", "", "Print timers every time-step.");
  printf("  %2s %14s %s\n", "-v", "[12]", "Increase the level of verbosity:");
  printf("  %2s %14s %s\n", "", "", "1: MPI-rank 0 writes,");
  printf("  %2s %14s %s\n", "", "", "2: All MPI-ranks write.");
  printf("  %2s %14s %s\n", "-y", "{int}",
         "Time-step frequency at which task graphs are dumped.");
  printf("  %2s %14s %s\n", "-Y", "{int}",
         "Time-step frequency at which threadpool tasks are dumped.");
  printf("  %2s %14s %s\n", "-h", "", "Print this help message and exit.");
  printf(
      "\nSee the file parameter_example.yml for an example of "
      "parameter file.\n");
}

/**
 * @brief Main routine that loads a few particles and generates some output.
 *
 */
int main(int argc, char *argv[]) {

  struct clocks_time tic, toc;
  struct engine e;

  /* Structs used by the engine. Declare now to make sure these are always in
   * scope.  */
  struct chemistry_global_data chemistry;
  struct cooling_function_data cooling_func;
  struct cosmology cosmo;
  struct external_potential potential;
  struct pm_mesh mesh;
  struct gpart *gparts = NULL;
  struct gravity_props gravity_properties;
  struct hydro_props hydro_properties;
  struct part *parts = NULL;
  struct phys_const prog_const;
  struct sourceterms sourceterms;
  struct space s;
  struct spart *sparts = NULL;
  struct unit_system us;

  int nr_nodes = 1, myrank = 0;

#ifdef WITH_MPI
  /* Start by initializing MPI. */
  int res = 0, prov = 0;
  if ((res = MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &prov)) !=
      MPI_SUCCESS)
    error("Call to MPI_Init failed with error %i.", res);
  if (prov != MPI_THREAD_MULTIPLE)
    error(
        "MPI does not provide the level of threading"
        " required (MPI_THREAD_MULTIPLE).");
  if ((res = MPI_Comm_size(MPI_COMM_WORLD, &nr_nodes)) != MPI_SUCCESS)
    error("MPI_Comm_size failed with error %i.", res);
  if ((res = MPI_Comm_rank(MPI_COMM_WORLD, &myrank)) != MPI_SUCCESS)
    error("Call to MPI_Comm_rank failed with error %i.", res);

  /* Make sure messages are stamped with the correct rank. */
  engine_rank = myrank;

  if ((res = MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN)) !=
      MPI_SUCCESS)
    error("Call to MPI_Comm_set_errhandler failed with error %i.", res);
  if (myrank == 0)
    printf("[0000] [00000.0] main: MPI is up and running with %i node(s).\n\n",
           nr_nodes);
  if (nr_nodes == 1) {
    message("WARNING: you are running with one MPI rank.");
    message("WARNING: you should use the non-MPI version of this program.");
  }
  fflush(stdout);

#endif

  /* Welcome to SWIFT, you made the right choice */
  if (myrank == 0) greetings();

  int with_aff = 0;
  int dry_run = 0;
  int dump_tasks = 0;
  int dump_threadpool = 0;
  int nsteps = -2;
  int restart = 0;
  int with_cosmology = 0;
  int with_external_gravity = 0;
  int with_sourceterms = 0;
  int with_cooling = 0;
  int with_self_gravity = 0;
  int with_hydro = 0;
  int with_stars = 0;
  int with_fp_exceptions = 0;
  int with_drift_all = 0;
  int with_mpole_reconstruction = 0;
  int verbose = 0;
  int nr_threads = 1;
  int with_verbose_timers = 0;
  int nparams = 0;
  char output_parameters_filename[200] = "";
  char *cmdparams[PARSER_MAX_NO_OF_PARAMS];
  char paramFileName[200] = "";
  char restart_file[200] = "";
  unsigned long long cpufreq = 0;

  /* Parse the parameters */
  int c;
  while ((c = getopt(argc, argv, "acCdDef:FgGhMn:o:P:rsSt:Tv:y:Y:")) != -1)
    switch (c) {
      case 'a':
#if defined(HAVE_SETAFFINITY) && defined(HAVE_LIBNUMA)
        with_aff = 1;
#else
        error("Need NUMA support for thread affinity");
#endif
        break;
      case 'c':
        with_cosmology = 1;
        break;
      case 'C':
        with_cooling = 1;
        break;
      case 'd':
        dry_run = 1;
        break;
      case 'D':
        with_drift_all = 1;
        break;
      case 'e':
#ifdef HAVE_FE_ENABLE_EXCEPT
        with_fp_exceptions = 1;
#else
        error("Need support for floating point exception on this platform");
#endif
        break;
      case 'f':
        if (sscanf(optarg, "%llu", &cpufreq) != 1) {
          if (myrank == 0) printf("Error parsing CPU frequency (-f).\n");
          if (myrank == 0) print_help_message();
          return 1;
        }
        break;
      case 'F':
        with_sourceterms = 1;
        break;
      case 'g':
        with_external_gravity = 1;
        break;
      case 'G':
        with_self_gravity = 1;
        break;
      case 'h':
        if (myrank == 0) print_help_message();
        return 0;
      case 'M':
        with_mpole_reconstruction = 1;
        break;
      case 'n':
        if (sscanf(optarg, "%d", &nsteps) != 1) {
          if (myrank == 0) printf("Error parsing fixed number of steps.\n");
          if (myrank == 0) print_help_message();
          return 1;
        }
        break;
      case 'o':
        if (sscanf(optarg, "%s", output_parameters_filename) != 1) {
          if (myrank == 0) {
            printf("Error parsing output fields filename");
            print_help_message();
          }
          return 1;
        }
        break;
      case 'P':
        cmdparams[nparams] = optarg;
        nparams++;
        break;
      case 'r':
        restart = 1;
        break;
      case 's':
        with_hydro = 1;
        break;
      case 'S':
        with_stars = 1;
        break;
      case 't':
        if (sscanf(optarg, "%d", &nr_threads) != 1) {
          if (myrank == 0)
            printf("Error parsing the number of threads (-t).\n");
          if (myrank == 0) print_help_message();
          return 1;
        }
        break;
      case 'T':
        with_verbose_timers = 1;
        break;
      case 'v':
        if (sscanf(optarg, "%d", &verbose) != 1) {
          if (myrank == 0) printf("Error parsing verbosity level (-v).\n");
          if (myrank == 0) print_help_message();
          return 1;
        }
        break;
      case 'y':
        if (sscanf(optarg, "%d", &dump_tasks) != 1) {
          if (myrank == 0) printf("Error parsing dump_tasks (-y). \n");
          if (myrank == 0) print_help_message();
          return 1;
        }
#ifndef SWIFT_DEBUG_TASKS
        if (dump_tasks) {
          error(
              "Task dumping is only possible if SWIFT was configured with the "
              "--enable-task-debugging option.");
        }
#endif
        break;
      case 'Y':
        if (sscanf(optarg, "%d", &dump_threadpool) != 1) {
          if (myrank == 0) printf("Error parsing dump_threadpool (-Y). \n");
          if (myrank == 0) print_help_message();
          return 1;
        }
#ifndef SWIFT_DEBUG_THREADPOOL
        if (dump_threadpool) {
          error(
              "Threadpool dumping is only possible if SWIFT was configured "
              "with the "
              "--enable-threadpool-debugging option.");
        }
#endif
        break;
      case '?':
        if (myrank == 0) print_help_message();
        return 1;
        break;
    }

  /* Write output parameter file */
  if (myrank == 0 && strcmp(output_parameters_filename, "") != 0) {
    io_write_output_field_parameter(output_parameters_filename);
    printf("End of run.\n");
    return 0;
  }

  /* check inputs */
  if (optind == argc - 1) {
    if (!strcpy(paramFileName, argv[optind++]))
      error("Error reading parameter file name.");
  } else if (optind > argc - 1) {
    if (myrank == 0) printf("Error: A parameter file name must be provided\n");
    if (myrank == 0) print_help_message();
    return 1;
  } else {
    if (myrank == 0) printf("Error: Too many parameters given\n");
    if (myrank == 0) print_help_message();
    return 1;
  }
  if (!with_self_gravity && !with_hydro && !with_external_gravity) {
    if (myrank == 0)
      printf("Error: At least one of -s, -g or -G must be chosen.\n");
    if (myrank == 0) print_help_message();
    return 1;
  }
  if (with_stars && !with_external_gravity && !with_self_gravity) {
    if (myrank == 0)
      printf(
          "Error: Cannot process stars without gravity, -g or -G must be "
          "chosen.\n");
    if (myrank == 0) print_help_message();
    return 1;
  }

/* Let's pin the main thread, now we know if affinity will be used. */
#if defined(HAVE_SETAFFINITY) && defined(HAVE_LIBNUMA) && defined(_GNU_SOURCE)
  if (with_aff &&
      ((ENGINE_POLICY)&engine_policy_setaffinity) == engine_policy_setaffinity)
    engine_pin();
#endif

  /* Genesis 1.1: And then, there was time ! */
  clocks_set_cpufreq(cpufreq);

  /* How vocal are we ? */
  const int talking = (verbose == 1 && myrank == 0) || (verbose == 2);

  if (myrank == 0 && dry_run)
    message(
        "Executing a dry run. No i/o or time integration will be performed.");

  /* Report CPU frequency.*/
  cpufreq = clocks_get_cpufreq();
  if (myrank == 0) {
    message("CPU frequency used for tick conversion: %llu Hz", cpufreq);
  }

/* Report host name(s). */
#ifdef WITH_MPI
  if (talking) {
    message("Rank %d running on: %s", myrank, hostname());
  }
#else
  message("Running on: %s", hostname());
#endif

/* Do we have debugging checks ? */
#ifdef SWIFT_DEBUG_CHECKS
  if (myrank == 0)
    message("WARNING: Debugging checks activated. Code will be slower !");
#endif

/* Do we have debugging checks ? */
#ifdef SWIFT_USE_NAIVE_INTERACTIONS
  if (myrank == 0)
    message(
        "WARNING: Naive cell interactions activated. Code will be slower !");
#endif

/* Do we have gravity accuracy checks ? */
#ifdef SWIFT_GRAVITY_FORCE_CHECKS
  if (myrank == 0)
    message(
        "WARNING: Checking 1/%d of all gpart for gravity accuracy. Code will "
        "be slower !",
        SWIFT_GRAVITY_FORCE_CHECKS);
#endif

  /* Do we choke on FP-exceptions ? */
  if (with_fp_exceptions) {
#ifdef HAVE_FE_ENABLE_EXCEPT
    feenableexcept(FE_DIVBYZERO | FE_INVALID | FE_OVERFLOW);
#endif
    if (myrank == 0)
      message("WARNING: Floating point exceptions will be reported.");
  }

/* Do we have slow barriers? */
#ifndef HAVE_PTHREAD_BARRIERS
  if (myrank == 0)
    message("WARNING: Non-optimal thread barriers are being used.");
#endif

  /* How large are the parts? */
  if (myrank == 0) {
    message("sizeof(part)        is %4zi bytes.", sizeof(struct part));
    message("sizeof(xpart)       is %4zi bytes.", sizeof(struct xpart));
    message("sizeof(spart)       is %4zi bytes.", sizeof(struct spart));
    message("sizeof(gpart)       is %4zi bytes.", sizeof(struct gpart));
    message("sizeof(multipole)   is %4zi bytes.", sizeof(struct multipole));
    message("sizeof(grav_tensor) is %4zi bytes.", sizeof(struct grav_tensor));
    message("sizeof(task)        is %4zi bytes.", sizeof(struct task));
    message("sizeof(cell)        is %4zi bytes.", sizeof(struct cell));
  }

  /* Read the parameter file */
  struct swift_params *params =
      (struct swift_params *)malloc(sizeof(struct swift_params));
  if (params == NULL) error("Error allocating memory for the parameter file.");
  if (myrank == 0) {
    message("Reading runtime parameters from file '%s'", paramFileName);
    parser_read_file(paramFileName, params);

    /* Handle any command-line overrides. */
    if (nparams > 0) {
      message(
          "Overwriting values read from the YAML file with command-line "
          "values.");
      for (int k = 0; k < nparams; k++) parser_set_param(params, cmdparams[k]);
    }
  }
#ifdef WITH_MPI
  /* Broadcast the parameter file */
  MPI_Bcast(params, sizeof(struct swift_params), MPI_BYTE, 0, MPI_COMM_WORLD);
#endif

  /* Check that we can write the snapshots by testing if the output
   * directory exists and is searchable and writable. */
  char basename[PARSER_MAX_LINE_SIZE];
  parser_get_param_string(params, "Snapshots:basename", basename);
  const char *dirp = dirname(basename);
  if (access(dirp, W_OK | X_OK) != 0) {
    error("Cannot write snapshots in directory %s (%s)", dirp, strerror(errno));
  }

  /* Prepare the domain decomposition scheme */
  struct repartition reparttype;
#ifdef WITH_MPI
  struct partition initial_partition;
  partition_init(&initial_partition, &reparttype, params, nr_nodes);

  /* Let's report what we did */
  if (myrank == 0) {
    message("Using initial partition %s",
            initial_partition_name[initial_partition.type]);
    if (initial_partition.type == INITPART_GRID)
      message("grid set to [ %i %i %i ].", initial_partition.grid[0],
              initial_partition.grid[1], initial_partition.grid[2]);
    message("Using %s repartitioning", repartition_name[reparttype.type]);
  }
#endif

  /* Common variables for restart and IC sections. */
  int clean_smoothing_length_values = 0;
  int flag_entropy_ICs = 0;

  /* Work out where we will read and write restart files. */
  char restart_dir[PARSER_MAX_LINE_SIZE];
  parser_get_opt_param_string(params, "Restarts:subdir", restart_dir,
                              "restart");

  /* The directory must exist. */
  if (myrank == 0) {
    if (access(restart_dir, W_OK | X_OK) != 0) {
      if (restart) {
        error("Cannot restart as no restart subdirectory: %s (%s)", restart_dir,
              strerror(errno));
      } else {
        if (mkdir(restart_dir, 0777) != 0)
          error("Failed to create restart directory: %s (%s)", restart_dir,
                strerror(errno));
      }
    }
  }

  /* Basename for any restart files. */
  char restart_name[PARSER_MAX_LINE_SIZE];
  parser_get_opt_param_string(params, "Restarts:basename", restart_name,
                              "swift");

  /* How often to check for the stop file and dump restarts and exit the
   * application. */
  int restart_stop_steps =
      parser_get_opt_param_int(params, "Restarts:stop_steps", 100);

  /* If restarting, look for the restart files. */
  if (restart) {

    /* Attempting a restart. */
    char **restart_files = NULL;
    int restart_nfiles = 0;

    if (myrank == 0) {
      message("Restarting SWIFT");

      /* Locate the restart files. */
      restart_files =
          restart_locate(restart_dir, restart_name, &restart_nfiles);
      if (restart_nfiles == 0)
        error("Failed to locate any restart files in %s", restart_dir);

      /* We need one file per rank. */
      if (restart_nfiles != nr_nodes)
        error("Incorrect number of restart files, expected %d found %d",
              nr_nodes, restart_nfiles);

      if (verbose > 0)
        for (int i = 0; i < restart_nfiles; i++)
          message("found restart file: %s", restart_files[i]);
    }

#ifdef WITH_MPI
    /* Distribute the restart files, need one for each rank. */
    if (myrank == 0) {

      for (int i = 1; i < nr_nodes; i++) {
        strcpy(restart_file, restart_files[i]);
        MPI_Send(restart_file, 200, MPI_BYTE, i, 0, MPI_COMM_WORLD);
      }

      /* Keep local file. */
      strcpy(restart_file, restart_files[0]);

      /* Finished with the list. */
      restart_locate_free(restart_nfiles, restart_files);

    } else {
      MPI_Recv(restart_file, 200, MPI_BYTE, 0, 0, MPI_COMM_WORLD,
               MPI_STATUS_IGNORE);
    }
    if (verbose > 1) message("local restart file = %s", restart_file);
#else

    /* Just one restart file. */
    strcpy(restart_file, restart_files[0]);
#endif

    /* Now read it. */
    restart_read(&e, restart_file);

    /* And initialize the engine with the space and policies. */
    if (myrank == 0) clocks_gettime(&tic);
    engine_config(1, &e, params, nr_nodes, myrank, nr_threads, with_aff,
                  talking, restart_file);
    if (myrank == 0) {
      clocks_gettime(&toc);
      message("engine_config took %.3f %s.", clocks_diff(&tic, &toc),
              clocks_getunit());
      fflush(stdout);
    }

    /* Check if we are already done when given steps on the command-line. */
    if (e.step >= nsteps && nsteps > 0)
      error("Not restarting, already completed %d steps", e.step);

  } else {

    /* Not restarting so look for the ICs. */
    /* Initialize unit system and constants */
    units_init_from_params(&us, params, "InternalUnitSystem");
    phys_const_init(&us, params, &prog_const);
    if (myrank == 0 && verbose > 0) {
      message("Internal unit system: U_M = %e g.", us.UnitMass_in_cgs);
      message("Internal unit system: U_L = %e cm.", us.UnitLength_in_cgs);
      message("Internal unit system: U_t = %e s.", us.UnitTime_in_cgs);
      message("Internal unit system: U_I = %e A.", us.UnitCurrent_in_cgs);
      message("Internal unit system: U_T = %e K.", us.UnitTemperature_in_cgs);
      phys_const_print(&prog_const);
    }

    /* Initialise the cosmology */
    if (with_cosmology)
      cosmology_init(params, &us, &prog_const, &cosmo);
    else
      cosmology_init_no_cosmo(&cosmo);
    if (myrank == 0 && with_cosmology) cosmology_print(&cosmo);

    /* Initialise the hydro properties */
    if (with_hydro)
      hydro_props_init(&hydro_properties, &prog_const, &us, params);
    if (with_hydro) eos_init(&eos, &prog_const, &us, params);

    /* Initialise the gravity properties */
    if (with_self_gravity)
      gravity_props_init(&gravity_properties, params, &cosmo, with_cosmology);

    /* Read particles and space information from (GADGET) ICs */
    char ICfileName[200] = "";
    parser_get_param_string(params, "InitialConditions:file_name", ICfileName);
    const int replicate =
        parser_get_opt_param_int(params, "InitialConditions:replicate", 1);
    clean_smoothing_length_values = parser_get_opt_param_int(
        params, "InitialConditions:cleanup_smoothing_lengths", 0);
    const int cleanup_h = parser_get_opt_param_int(
        params, "InitialConditions:cleanup_h_factors", 0);
    const int cleanup_sqrt_a = parser_get_opt_param_int(
        params, "InitialConditions:cleanup_velocity_factors", 0);
    const int generate_gas_in_ics = parser_get_opt_param_int(
        params, "InitialConditions:generate_gas_in_ics", 0);
    if (generate_gas_in_ics && flag_entropy_ICs)
      error("Can't generate gas if the entropy flag is set in the ICs.");
    if (generate_gas_in_ics && !with_cosmology)
      error("Can't generate gas if the run is not cosmological.");
    if (myrank == 0) message("Reading ICs from file '%s'", ICfileName);
    if (myrank == 0 && cleanup_h)
      message("Cleaning up h-factors (h=%f)", cosmo.h);
    if (myrank == 0 && cleanup_sqrt_a)
      message("Cleaning up a-factors from velocity (a=%f)", cosmo.a);
    fflush(stdout);

    /* Get ready to read particles of all kinds */
    size_t Ngas = 0, Ngpart = 0, Nspart = 0;
    double dim[3] = {0., 0., 0.};
    int periodic = 0;
    if (myrank == 0) clocks_gettime(&tic);
#if defined(HAVE_HDF5)
#if defined(WITH_MPI)
#if defined(HAVE_PARALLEL_HDF5)
    read_ic_parallel(ICfileName, &us, dim, &parts, &gparts, &sparts, &Ngas,
                     &Ngpart, &Nspart, &periodic, &flag_entropy_ICs, with_hydro,
                     (with_external_gravity || with_self_gravity), with_stars,
                     cleanup_h, cleanup_sqrt_a, cosmo.h, cosmo.a, myrank,
                     nr_nodes, MPI_COMM_WORLD, MPI_INFO_NULL, nr_threads,
                     dry_run);
#else
    read_ic_serial(ICfileName, &us, dim, &parts, &gparts, &sparts, &Ngas,
                   &Ngpart, &Nspart, &periodic, &flag_entropy_ICs, with_hydro,
                   (with_external_gravity || with_self_gravity), with_stars,
                   cleanup_h, cleanup_sqrt_a, cosmo.h, cosmo.a, myrank,
                   nr_nodes, MPI_COMM_WORLD, MPI_INFO_NULL, nr_threads,
                   dry_run);
#endif
#else
    read_ic_single(ICfileName, &us, dim, &parts, &gparts, &sparts, &Ngas,
                   &Ngpart, &Nspart, &periodic, &flag_entropy_ICs, with_hydro,
                   (with_external_gravity || with_self_gravity), with_stars,
                   cleanup_h, cleanup_sqrt_a, cosmo.h, cosmo.a, nr_threads,
                   dry_run);
#endif
#endif
    if (myrank == 0) {
      clocks_gettime(&toc);
      message("Reading initial conditions took %.3f %s.",
              clocks_diff(&tic, &toc), clocks_getunit());
      fflush(stdout);
    }

#ifdef WITH_MPI
    if (periodic && with_self_gravity)
      error("Periodic self-gravity over MPI temporarily disabled.");
#endif

#ifdef SWIFT_DEBUG_CHECKS
    /* Check once and for all that we don't have unwanted links */
    if (!with_stars && !dry_run) {
      for (size_t k = 0; k < Ngpart; ++k)
        if (gparts[k].type == swift_type_star) error("Linking problem");
    }
    if (!with_hydro && !dry_run) {
      for (size_t k = 0; k < Ngpart; ++k)
        if (gparts[k].type == swift_type_gas) error("Linking problem");
    }

    /* Check that the other links are correctly set */
    if (!dry_run)
      part_verify_links(parts, gparts, sparts, Ngas, Ngpart, Nspart, 1);
#endif

    /* Get the total number of particles across all nodes. */
    long long N_total[3] = {0, 0, 0};
#if defined(WITH_MPI)
    long long N_long[3] = {Ngas, Ngpart, Nspart};
    MPI_Allreduce(&N_long, &N_total, 3, MPI_LONG_LONG_INT, MPI_SUM,
                  MPI_COMM_WORLD);
#else
    N_total[0] = Ngas;
    N_total[1] = Ngpart;
    N_total[2] = Nspart;
#endif

    if (myrank == 0)
      message(
          "Read %lld gas particles, %lld star particles and %lld gparts from "
          "the "
          "ICs.",
          N_total[0], N_total[2], N_total[1]);

    /* Verify that the fields to dump actually exist */
    if (myrank == 0) io_check_output_fields(params, N_total);

    /* Initialise the long-range gravity mesh */
    if (with_self_gravity && periodic) {
#ifdef HAVE_FFTW
      pm_mesh_init(&mesh, &gravity_properties, dim);
#else
      /* Need the FFTW library if periodic and self gravity. */
      error(
          "No FFTW library found. Cannot compute periodic long-range forces.");
#endif
    } else {
      pm_mesh_init_no_mesh(&mesh, dim);
    }

    /* Initialize the space with these data. */
    if (myrank == 0) clocks_gettime(&tic);
    space_init(&s, params, &cosmo, dim, parts, gparts, sparts, Ngas, Ngpart,
               Nspart, periodic, replicate, generate_gas_in_ics,
               with_self_gravity, talking, dry_run);

    if (myrank == 0) {
      clocks_gettime(&toc);
      message("space_init took %.3f %s.", clocks_diff(&tic, &toc),
              clocks_getunit());
      fflush(stdout);
    }

    /* Check that the matter content matches the cosmology given in the
     * parameter file. */
    if (with_cosmology && with_self_gravity && !dry_run)
      space_check_cosmology(&s, &cosmo, myrank);

/* Also update the total counts (in case of changes due to replication) */
#if defined(WITH_MPI)
    N_long[0] = s.nr_parts;
    N_long[1] = s.nr_gparts;
    N_long[2] = s.nr_sparts;
    MPI_Allreduce(&N_long, &N_total, 3, MPI_LONG_LONG_INT, MPI_SUM,
                  MPI_COMM_WORLD);
#else
    N_total[0] = s.nr_parts;
    N_total[1] = s.nr_gparts;
    N_total[2] = s.nr_sparts;
#endif

    /* Say a few nice things about the space we just created. */
    if (myrank == 0) {
      message("space dimensions are [ %.3f %.3f %.3f ].", s.dim[0], s.dim[1],
              s.dim[2]);
      message("space %s periodic.", s.periodic ? "is" : "isn't");
      message("highest-level cell dimensions are [ %i %i %i ].", s.cdim[0],
              s.cdim[1], s.cdim[2]);
      message("%zi parts in %i cells.", s.nr_parts, s.tot_cells);
      message("%zi gparts in %i cells.", s.nr_gparts, s.tot_cells);
      message("%zi sparts in %i cells.", s.nr_sparts, s.tot_cells);
      message("maximum depth is %d.", s.maxdepth);
      fflush(stdout);
    }

    /* Verify that each particle is in it's proper cell. */
    if (talking && !dry_run) {
      int icount = 0;
      space_map_cells_pre(&s, 0, &map_cellcheck, &icount);
      message("map_cellcheck picked up %i parts.", icount);
    }

    /* Verify the maximal depth of cells. */
    if (talking && !dry_run) {
      int data[2] = {s.maxdepth, 0};
      space_map_cells_pre(&s, 0, &map_maxdepth, data);
      message("nr of cells at depth %i is %i.", data[0], data[1]);
    }

    /* Initialise the external potential properties */
    if (with_external_gravity)
      potential_init(params, &prog_const, &us, &s, &potential);
    if (myrank == 0) potential_print(&potential);

    /* Initialise the cooling function properties */
    if (with_cooling) cooling_init(params, &us, &prog_const, &cooling_func);
    if (myrank == 0) cooling_print(&cooling_func);

    /* Initialise the chemistry */
    chemistry_init(params, &us, &prog_const, &chemistry);
    if (myrank == 0) chemistry_print(&chemistry);

    /* Initialise the feedback properties */
    if (with_sourceterms) sourceterms_init(params, &us, &sourceterms);
    if (with_sourceterms && myrank == 0) sourceterms_print(&sourceterms);

    /* Construct the engine policy */
    int engine_policies = ENGINE_POLICY | engine_policy_steal;
    if (with_drift_all) engine_policies |= engine_policy_drift_all;
    if (with_mpole_reconstruction)
      engine_policies |= engine_policy_reconstruct_mpoles;
    if (with_hydro) engine_policies |= engine_policy_hydro;
    if (with_self_gravity) engine_policies |= engine_policy_self_gravity;
    if (with_external_gravity)
      engine_policies |= engine_policy_external_gravity;
    if (with_cosmology) engine_policies |= engine_policy_cosmology;
    if (with_cooling) engine_policies |= engine_policy_cooling;
    if (with_sourceterms) engine_policies |= engine_policy_sourceterms;
    if (with_stars) engine_policies |= engine_policy_stars;

    /* Initialize the engine with the space and policies. */
    if (myrank == 0) clocks_gettime(&tic);
    engine_init(&e, &s, params, N_total[0], N_total[1], N_total[2],
                engine_policies, talking, &reparttype, &us, &prog_const, &cosmo,
                &hydro_properties, &gravity_properties, &mesh, &potential,
                &cooling_func, &chemistry, &sourceterms);
    engine_config(0, &e, params, nr_nodes, myrank, nr_threads, with_aff,
                  talking, restart_file);

    if (myrank == 0) {
      clocks_gettime(&toc);
      message("engine_init took %.3f %s.", clocks_diff(&tic, &toc),
              clocks_getunit());
      fflush(stdout);
    }

    /* Get some info to the user. */
    if (myrank == 0) {
      long long N_DM = N_total[1] - N_total[2] - N_total[0];
      message(
          "Running on %lld gas particles, %lld star particles and %lld DM "
          "particles (%lld gravity particles)",
          N_total[0], N_total[2], N_total[1] > 0 ? N_DM : 0, N_total[1]);
      message(
          "from t=%.3e until t=%.3e with %d threads and %d queues "
          "(dt_min=%.3e, "
          "dt_max=%.3e)...",
          e.time_begin, e.time_end, e.nr_threads, e.sched.nr_queues, e.dt_min,
          e.dt_max);
      fflush(stdout);
    }
  }

  /* Time to say good-bye if this was not a serious run. */
  if (dry_run) {
#ifdef WITH_MPI
    if ((res = MPI_Finalize()) != MPI_SUCCESS)
      error("call to MPI_Finalize failed with error %i.", res);
#endif
    if (myrank == 0)
      message("Time integration ready to start. End of dry-run.");
    engine_clean(&e);
    free(params);
    return 0;
  }

/* Initialise the table of Ewald corrections for the gravity checks */
#ifdef SWIFT_GRAVITY_FORCE_CHECKS
  if (s.periodic) gravity_exact_force_ewald_init(e.s->dim[0]);
#endif

/* Init the runner history. */
#ifdef HIST
  for (k = 0; k < runner_hist_N; k++) runner_hist_bins[k] = 0;
#endif

  if (!restart) {

#ifdef WITH_MPI
    /* Split the space. */
    engine_split(&e, &initial_partition);
    engine_redistribute(&e);
#endif

    /* Initialise the particles */
    engine_init_particles(&e, flag_entropy_ICs, clean_smoothing_length_values);

    /* Write the state of the system before starting time integration. */
    engine_dump_snapshot(&e);
    engine_print_stats(&e);
  }

  /* Legend */
  if (myrank == 0)
    printf("# %6s %14s %14s %10s %14s %9s %12s %12s %12s %16s [%s] %6s\n",
           "Step", "Time", "Scale-factor", "Redshift", "Time-step", "Time-bins",
           "Updates", "g-Updates", "s-Updates", "Wall-clock time",
           clocks_getunit(), "Props");

  /* File for the timers */
  if (with_verbose_timers) timers_open_file(myrank);

  /* Create a name for restart file of this rank. */
  if (restart_genname(restart_dir, restart_name, e.nodeID, restart_file, 200) !=
      0)
    error("Failed to generate restart filename");

  /* dump the parameters as used. */

  /* used parameters */
  parser_write_params_to_file(params, "used_parameters.yml", 1);
  /* unused parameters */
  parser_write_params_to_file(params, "unused_parameters.yml", 0);

  /* Main simulation loop */
  /* ==================== */
  int force_stop = 0;
  for (int j = 0; !engine_is_done(&e) && e.step - 1 != nsteps && !force_stop;
       j++) {

    /* Reset timers */
    timers_reset_all();

    /* Take a step. */
    engine_step(&e);

    /* Print the timers. */
    if (with_verbose_timers) timers_print(e.step);

    /* Every so often allow the user to stop the application and dump the
     * restart files. */
    if (j % restart_stop_steps == 0) {
      force_stop = restart_stop_now(restart_dir, 0);
      if (myrank == 0 && force_stop)
        message("Forcing application exit, dumping restart files...");
    }

    /* Also if using nsteps to exit, will not have saved any restarts on exit,
     * make sure we do that (useful in testing only). */
    if (force_stop || (e.restart_onexit && e.step - 1 == nsteps))
      engine_dump_restarts(&e, 0, 1);

#ifdef SWIFT_DEBUG_TASKS
    /* Dump the task data using the given frequency. */
    if (dump_tasks && (dump_tasks == 1 || j % dump_tasks == 1)) {
#ifdef WITH_MPI

      /* Make sure output file is empty, only on one rank. */
      char dumpfile[30];
      snprintf(dumpfile, 30, "thread_info_MPI-step%d.dat", j + 1);
      FILE *file_thread;
      if (myrank == 0) {
        file_thread = fopen(dumpfile, "w");
        fclose(file_thread);
      }
      MPI_Barrier(MPI_COMM_WORLD);

      for (int i = 0; i < nr_nodes; i++) {

        /* Rank 0 decides the index of writing node, this happens one-by-one. */
        int kk = i;
        MPI_Bcast(&kk, 1, MPI_INT, 0, MPI_COMM_WORLD);

        if (i == myrank) {

          /* Open file and position at end. */
          file_thread = fopen(dumpfile, "a");

          fprintf(file_thread,
                  " %03d 0 0 0 0 %lld %lld %lld %lld %lld 0 0 %lld\n", myrank,
                  e.tic_step, e.toc_step, e.updates, e.g_updates, e.s_updates,
                  cpufreq);
          int count = 0;
          for (int l = 0; l < e.sched.nr_tasks; l++) {
            if (!e.sched.tasks[l].implicit && e.sched.tasks[l].toc != 0) {
              fprintf(
                  file_thread,
                  " %03i %i %i %i %i %lli %lli %i %i %i %i %i %i\n", myrank,
                  e.sched.tasks[l].rid, e.sched.tasks[l].type,
                  e.sched.tasks[l].subtype, (e.sched.tasks[l].cj == NULL),
                  e.sched.tasks[l].tic, e.sched.tasks[l].toc,
                  (e.sched.tasks[l].ci != NULL) ? e.sched.tasks[l].ci->count
                                                : 0,
                  (e.sched.tasks[l].cj != NULL) ? e.sched.tasks[l].cj->count
                                                : 0,
                  (e.sched.tasks[l].ci != NULL) ? e.sched.tasks[l].ci->gcount
                                                : 0,
                  (e.sched.tasks[l].cj != NULL) ? e.sched.tasks[l].cj->gcount
                                                : 0,
                  e.sched.tasks[l].flags, e.sched.tasks[l].sid);
            }
            fflush(stdout);
            count++;
          }
          fclose(file_thread);
        }

        /* And we wait for all to synchronize. */
        MPI_Barrier(MPI_COMM_WORLD);
      }

#else
      char dumpfile[30];
      snprintf(dumpfile, 30, "thread_info-step%d.dat", j + 1);
      FILE *file_thread;
      file_thread = fopen(dumpfile, "w");
      /* Add some information to help with the plots */
      fprintf(file_thread, " %d %d %d %d %lld %lld %lld %lld %lld %d %lld\n",
              -2, -1, -1, 1, e.tic_step, e.toc_step, e.updates, e.g_updates,
              e.s_updates, 0, cpufreq);
      for (int l = 0; l < e.sched.nr_tasks; l++) {
        if (!e.sched.tasks[l].implicit && e.sched.tasks[l].toc != 0) {
          fprintf(
              file_thread, " %i %i %i %i %lli %lli %i %i %i %i %i\n",
              e.sched.tasks[l].rid, e.sched.tasks[l].type,
              e.sched.tasks[l].subtype, (e.sched.tasks[l].cj == NULL),
              e.sched.tasks[l].tic, e.sched.tasks[l].toc,
              (e.sched.tasks[l].ci == NULL) ? 0 : e.sched.tasks[l].ci->count,
              (e.sched.tasks[l].cj == NULL) ? 0 : e.sched.tasks[l].cj->count,
              (e.sched.tasks[l].ci == NULL) ? 0 : e.sched.tasks[l].ci->gcount,
              (e.sched.tasks[l].cj == NULL) ? 0 : e.sched.tasks[l].cj->gcount,
              e.sched.tasks[l].sid);
        }
      }
      fclose(file_thread);
#endif  // WITH_MPI
    }
#endif  // SWIFT_DEBUG_TASKS

#ifdef SWIFT_DEBUG_THREADPOOL
    /* Dump the task data using the given frequency. */
    if (dump_threadpool && (dump_threadpool == 1 || j % dump_threadpool == 1)) {
      char dumpfile[40];
#ifdef WITH_MPI
      snprintf(dumpfile, 40, "threadpool_info-rank%d-step%d.dat", engine_rank,
               j + 1);
#else
      snprintf(dumpfile, 40, "threadpool_info-step%d.dat", j + 1);
#endif  // WITH_MPI
      threadpool_dump_log(&e.threadpool, dumpfile, 1);
    } else {
      threadpool_reset_log(&e.threadpool);
    }
#endif  // SWIFT_DEBUG_THREADPOOL
  }

/* Print the values of the runner histogram. */
#ifdef HIST
  printf("main: runner histogram data:\n");
  for (k = 0; k < runner_hist_N; k++)
    printf(" %e %e %e\n",
           runner_hist_a + k * (runner_hist_b - runner_hist_a) / runner_hist_N,
           runner_hist_a +
               (k + 1) * (runner_hist_b - runner_hist_a) / runner_hist_N,
           (double)runner_hist_bins[k]);
#endif

  /* Write final time information */
  if (myrank == 0) {

    /* Print some information to the screen */
    printf(
        "  %6d %14e %14e %10.5f %14e %4d %4d %12lld %12lld %12lld %21.3f %6d\n",
        e.step, e.time, e.cosmology->a, e.cosmology->z, e.time_step,
        e.min_active_bin, e.max_active_bin, e.updates, e.g_updates, e.s_updates,
        e.wallclock_time, e.step_props);
    fflush(stdout);

    fprintf(e.file_timesteps,
            "  %6d %14e %14e %14e %4d %4d %12lld %12lld %12lld %21.3f %6d\n",
            e.step, e.time, e.cosmology->a, e.time_step, e.min_active_bin,
            e.max_active_bin, e.updates, e.g_updates, e.s_updates,
            e.wallclock_time, e.step_props);
    fflush(e.file_timesteps);
  }

  /* Write final output. */
  engine_drift_all(&e);
  engine_print_stats(&e);
  engine_dump_snapshot(&e);

#ifdef WITH_MPI
  if ((res = MPI_Finalize()) != MPI_SUCCESS)
    error("call to MPI_Finalize failed with error %i.", res);
#endif

  /* Remove the stop file if used. Do this anyway, we could have missed the
   * stop file if normal exit happened first. */
  if (myrank == 0) force_stop = restart_stop_now(restart_dir, 1);

  /* Clean everything */
  if (with_verbose_timers) timers_close_file();
  if (with_cosmology) cosmology_clean(&cosmo);
  if (with_self_gravity) pm_mesh_clean(&mesh);
  engine_clean(&e);
  free(params);

  /* Say goodbye. */
  if (myrank == 0) message("done. Bye.");

  /* All is calm, all is bright. */
  return 0;
}
