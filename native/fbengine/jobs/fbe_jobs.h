#ifndef FBE_JOBS_H
#define FBE_JOBS_H

// The engine's jobs: work on worker threads, a worker per core less one, which loading, culling,
// animation, and collisions run on. Work the FB side waits for has a ticket, which the
// fbe_frame_flip after the work finishes hands over.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// How many worker threads the jobs run on: one per core less one, at least one.
FBE_API int32_t fbe_jobs_workers(void);
// How many tickets the last fbe_frame_flip handed over.
FBE_API int32_t fbe_jobs_finished_count(void);
// The ticket at `index` of those the last fbe_frame_flip handed over, in the order they were
// handed over; 0 if there's none there.
FBE_API uint64_t fbe_jobs_finished_at(int32_t index);
// Starts a job that takes `microseconds` on a worker, as a load would, and gives its ticket, which
// the flip after it finishes hands over; for tests. 0 if `microseconds` is below 0.
FBE_API uint64_t fbe_jobs_simulate(int32_t microseconds);

#ifdef __cplusplus
}
#endif

#endif
