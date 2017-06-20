#ifndef SWIFT_CUDA_RUNNER_MAIN_H
#define SWIFT_CUDA_RUNNER_MAIN_H

//We want 100% occupancy I think.
#define num_blocks 128
#define num_threads 128

__host__ void create_tasks(struct engine *e);
__host__ void create_cells_and_data_tasks(struct engine *e);
__host__ void run_cuda();




#endif
