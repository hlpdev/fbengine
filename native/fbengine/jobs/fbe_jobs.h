#ifndef FBE_JOBS_H
#define FBE_JOBS_H

// The engine's jobs: work on worker threads, a worker per core less one, which loading, culling,
// animation, and collisions run on. Work the FB side waits for has a ticket, which a hand-over
// after the work finishes gives the FB side, at fbe_frame_flip or fbe_jobs_hand_over.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// How many worker threads the jobs run on: one per core the process can run on, less one, and at
// least one.
FBE_API int32_t fbe_jobs_workers(void);
// Runs the work the jobs finished for the main thread, handing over the tickets of what's done,
// as fbe_frame_flip does: for the FB side's waits when it isn't flipping.
FBE_API void fbe_jobs_hand_over(void);
// How many times fbe_jobs_hand_over has been called: for tests of how often the FB side hands
// over while it waits.
FBE_API uint64_t fbe_jobs_hand_overs(void);
// How many tickets have been handed over since the FB side last forgot them.
FBE_API int32_t fbe_jobs_finished_count(void);
// The ticket at `index` of those, in the order they were handed over; 0 if there's none there.
FBE_API uint64_t fbe_jobs_finished_at(int32_t index);
// Forgets the tickets handed over, once the FB side has them.
FBE_API void fbe_jobs_forget_finished(void);
// Starts a job that takes `microseconds` on a worker, as a load would, and gives its ticket, which
// the hand-over after it finishes gives the FB side; for tests. 0 if `microseconds` is below 0.
FBE_API uint64_t fbe_jobs_simulate(int32_t microseconds);

#ifdef __cplusplus
}
#endif

#endif
