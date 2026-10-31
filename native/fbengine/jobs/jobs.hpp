#pragma once

// The engine's jobs: work on worker threads, which asset decoding, culling, animation, and
// collisions run on, and work handed back to the main thread at fbe_frame_flip. Nothing here calls
// into FB.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace fbe {

/// A job, as the C API's handles are: its slot and that slot's generation, so a finished job's
/// handle never stands for the job that takes its slot next. 0 is none, which has finished.
using job_t = std::uint64_t;

/// Worker threads and the jobs they run, each once, as soon as the jobs it runs after have
/// finished, in the order they became ready. Any thread can start a job or wait for one, a job
/// too. A job doesn't throw: one that does ends the program, as a thread's would.
class jobs_t {
public:
    /// Starts `workers` worker threads, at least one.
    explicit jobs_t(int workers);
    /// Waits for every job, those they start too, and runs the work handed to the main thread that
    /// hasn't run, in turn until neither has any left, so none is lost; then stops the workers.
    ~jobs_t();
    jobs_t(const jobs_t&) = delete;
    jobs_t& operator=(const jobs_t&) = delete;
    jobs_t(jobs_t&&) = delete;
    jobs_t& operator=(jobs_t&&) = delete;

    /// Runs `work` on a worker once every job in `after` has finished.
    job_t run(std::move_only_function<void()> work, std::span<const job_t> after = {});

    /// Runs `work(begin, end)` over 0 to `count` in ranges of at least `grain` items, on the
    /// workers at once, once every job in `after` has finished, and gives a job that finishes when
    /// every range has. There are at most four ranges a thread, so many small items cost few jobs.
    job_t run_for(std::size_t count, std::size_t grain,
                  std::function<void(std::size_t begin, std::size_t end)> work,
                  std::span<const job_t> after = {});

    /// Whether `job` has finished.
    [[nodiscard]] bool finished(job_t job) const;

    /// Waits for `job` to finish, running jobs that are ready on this thread meanwhile, so a job
    /// can wait for the jobs it starts.
    void wait(job_t job);

    /// Queues `work` for the next hand_over, from any thread: how a job gives what it made to the
    /// main thread.
    void to_main(std::move_only_function<void()> work);

    /// Runs the work queued for the main thread, in the order it was queued; work it queues waits
    /// for the next hand_over. fbe_frame_flip calls it for the engine's jobs.
    void hand_over();

    /// How many worker threads there are.
    [[nodiscard]] int workers() const;

private:
    struct slot_t {
        std::uint32_t generation = 0;
        // How many of the jobs it runs after haven't finished.
        std::uint32_t waiting = 0;
        std::move_only_function<void()> work;
        // The slots of the jobs that run after it.
        std::vector<std::uint32_t> then;
    };

    job_t add(std::unique_lock<std::mutex>& lock, std::move_only_function<void()> work,
              std::span<const job_t> after);
    [[nodiscard]] bool finished(const std::unique_lock<std::mutex>& lock, job_t job) const;
    void run_next(std::unique_lock<std::mutex>& lock) noexcept;
    void work_loop();

    mutable std::mutex m_mutex;
    // A job is ready, or the workers are to stop.
    std::condition_variable m_ready;
    // A job finished.
    std::condition_variable m_finished;
    std::vector<slot_t> m_slots;
    std::vector<std::uint32_t> m_free;
    // The slots of the jobs ready to run, first ready first.
    std::deque<std::uint32_t> m_queue;
    std::size_t m_unfinished = 0;
    bool m_stopping = false;
    std::vector<std::thread> m_threads;

    std::mutex m_main_mutex;
    std::vector<std::move_only_function<void()>> m_main;
};

/// How many workers the engine's jobs have: one per core less one, at least one.
int engine_workers();

/// The engine's jobs, started the first time they're asked for. They're never stopped, since a
/// DLL's statics end after the OS has ended its threads; the OS ends the workers with the program.
jobs_t& jobs();

/// Runs the work the engine's jobs handed to the main thread, for fbe_frame_flip, after
/// forgetting the tickets the last one handed over.
void hand_over_jobs();

/// A new ticket, for work the FB side waits for, never 0.
std::uint64_t new_ticket();

/// Hands `ticket` to the FB side at this flip, from work run by hand_over, on the main thread.
void hand_to_fb(std::uint64_t ticket);

} // namespace fbe
