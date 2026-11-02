#pragma once

// The engine's jobs: work on worker threads, which asset decoding, culling, animation, and
// collisions run on, and work handed back to the main thread, at fbe_frame_flip and whenever the
// FB side waits for some. Nothing here calls into FB.

#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace fbe {

/// Work to run once, which can hold what can't be copied: what std::move_only_function is, which
/// libc++ doesn't have. Empty, it's false.
class work_t {
public:
    work_t() = default;

    template <typename Work>
        requires(!std::same_as<std::remove_cvref_t<Work>, work_t> &&
                 std::invocable<std::decay_t<Work>&>)
    // A lambda is work, as it is a std::function's.
    work_t(Work&& work)
        : m_held(std::make_unique<held_t<std::decay_t<Work>>>(std::forward<Work>(work))) {}

    void operator()() {
        m_held->run();
    }

    explicit operator bool() const {
        return m_held != nullptr;
    }

private:
    struct base_t {
        base_t() = default;
        virtual ~base_t() = default;
        base_t(const base_t&) = delete;
        base_t& operator=(const base_t&) = delete;
        base_t(base_t&&) = delete;
        base_t& operator=(base_t&&) = delete;
        virtual void run() = 0;
    };

    template <typename Work>
    struct held_t final : base_t {
        explicit held_t(Work&& from) : work(std::move(from)) {}
        explicit held_t(const Work& from) : work(from) {}
        void run() override {
            std::invoke(work);
        }
        Work work;
    };

    std::unique_ptr<base_t> m_held;
};

/// A job, as the C API's handles are: its slot and that slot's generation, so a finished job's
/// handle never stands for the job that takes its slot next. 0 is none, which has finished.
using job_t = std::uint64_t;

/// Worker threads and the jobs they run, each once, as soon as the jobs it runs after have
/// finished, in the order they became ready. Any thread can start a job or wait for one, a job
/// too. A job doesn't throw: one that does ends the program, as a thread's would. A job never
/// waits for one that runs after it, which would wait forever.
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
    job_t run(work_t work, std::span<const job_t> after = {});

    /// Runs `work(begin, end)` over 0 to `count` in ranges of at least `grain` items (or one range,
    /// when there are fewer), on the workers at once, once every job in `after` has finished, and
    /// gives a job that finishes when every range has. There are at most four ranges a thread, so
    /// many small items cost few jobs.
    job_t run_for(std::size_t count, std::size_t grain,
                  std::function<void(std::size_t begin, std::size_t end)> work,
                  std::span<const job_t> after = {});

    /// Whether `job` has finished.
    [[nodiscard]] bool finished(job_t job) const;

    /// Waits for `job` to finish, running on this thread meanwhile what's ready of it and of the
    /// jobs it runs after, all the way back, and nothing else, so a wait ends soon after its job
    /// does, and a job can wait for the jobs it starts.
    void wait(job_t job);

    /// Queues `work` for the next hand_over, from any thread: how a job gives what it made to the
    /// main thread.
    void to_main(work_t work);

    /// Runs the work queued for the main thread, in the order it was queued; work it queues waits
    /// for the next hand_over.
    void hand_over();

    /// How many worker threads there are.
    [[nodiscard]] int workers() const;

private:
    enum class state_t : std::uint8_t { FREE, WAITING, READY, RUNNING };

    struct slot_t {
        std::uint32_t generation = 0;
        state_t state = state_t::FREE;
        // How many of the jobs it runs after haven't finished.
        std::uint32_t waiting = 0;
        // The last search for what a wait can run that came to it.
        std::uint64_t searched = 0;
        work_t work;
        // The jobs it runs after that hadn't finished when it was started.
        std::vector<job_t> after;
        // The slots of the jobs that run after it.
        std::vector<std::uint32_t> then;
    };

    job_t add(std::unique_lock<std::mutex>& lock, work_t work, std::span<const job_t> after);
    [[nodiscard]] bool finished(const std::unique_lock<std::mutex>& lock, job_t job) const;
    // The next ready job in the queue, taken out, of all of them.
    std::optional<std::uint32_t> next_ready(const std::unique_lock<std::mutex>& lock);
    // A ready job of `job` and those it runs after, or none.
    std::optional<std::uint32_t> ready_for(const std::unique_lock<std::mutex>& lock, job_t job);
    void run(std::unique_lock<std::mutex>& lock, std::uint32_t slot) noexcept;
    void work_loop();

    mutable std::mutex m_mutex;
    // A job is ready, or the workers are to stop.
    std::condition_variable m_ready;
    // A job finished.
    std::condition_variable m_finished;
    std::vector<slot_t> m_slots;
    std::vector<std::uint32_t> m_free;
    // The jobs ready to run, first ready first; one a wait ran, or that's finished, is passed over.
    std::deque<job_t> m_queue;
    std::size_t m_unfinished = 0;
    bool m_stopping = false;
    // How many threads have started, which the constructor waits for.
    int m_started = 0;
    std::condition_variable m_all_started;
    std::uint64_t m_searches = 0;
    // The jobs a search has still to look at.
    std::vector<job_t> m_search;
    std::vector<std::thread> m_threads;

    std::mutex m_main_mutex;
    std::vector<work_t> m_main;
};

/// How many workers the engine's jobs have: one per core this process can run on, less one, and
/// at least one.
int engine_workers();

/// The engine's jobs, started the first time they're asked for, and never stopped: on Windows a
/// DLL's statics end after the OS has ended its threads, and on Linux and macOS exit() ends a
/// library's statics while its threads still run. So the workers run until the program ends, and
/// a job never touches a static that has a destructor.
jobs_t& jobs();

/// Runs the work the engine's jobs handed to the main thread, which hands the FB side the tickets
/// of what it waits for: at fbe_frame_flip, and at fbe_jobs_hand_over.
void hand_over_jobs();

/// A new ticket, for work the FB side waits for, never 0.
std::uint64_t new_ticket();

/// Hands `ticket` to the FB side, from work hand_over_jobs runs, on the main thread.
void hand_to_fb(std::uint64_t ticket);

} // namespace fbe
