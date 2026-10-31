// jobs_t: one lock over the jobs, each in a slot that a finished job gives back, a queue of the
// ready ones, and threads that wait for one. A job's dependents are in its slot, so finishing it
// readies them without a search.

#include "jobs/jobs.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

namespace {

// Ranges a thread run_for makes at most, so a slow range leaves the others work to take.
constexpr std::size_t ranges_a_thread = 4;

std::uint32_t slot_of(fbe::job_t job) {
    return static_cast<std::uint32_t>(job & 0xFFFF'FFFFU) - 1;
}

std::uint32_t generation_of(fbe::job_t job) {
    return static_cast<std::uint32_t>(job >> 32U);
}

} // namespace

namespace fbe {

jobs_t::jobs_t(int workers) {
    const int count = std::max(workers, 1);
    m_threads.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        m_threads.emplace_back([this] { work_loop(); });
    }
}

jobs_t::~jobs_t() {
    // Work handed to the main thread can start jobs, so the two are run in turn until neither has
    // any left, before the workers stop.
    while (true) {
        {
            std::unique_lock lock(m_mutex);
            while (m_unfinished > 0) {
                if (!m_queue.empty()) {
                    run_next(lock);
                } else {
                    m_finished.wait(lock);
                }
            }
        }
        std::vector<std::move_only_function<void()>> left;
        {
            const std::scoped_lock lock(m_main_mutex);
            left.swap(m_main);
        }
        if (left.empty()) {
            break;
        }
        for (auto& work : left) {
            work();
        }
    }
    {
        const std::scoped_lock lock(m_mutex);
        m_stopping = true;
    }
    m_ready.notify_all();
    for (std::thread& thread : m_threads) {
        thread.join();
    }
}

job_t jobs_t::run(std::move_only_function<void()> work, std::span<const job_t> after) {
    std::unique_lock lock(m_mutex);
    return add(lock, std::move(work), after);
}

job_t jobs_t::run_for(std::size_t count, std::size_t grain,
                      std::function<void(std::size_t begin, std::size_t end)> work,
                      std::span<const job_t> after) {
    const std::size_t least = std::max(grain, std::size_t{ 1 });
    const std::size_t ranges =
        std::min(ranges_a_thread * (m_threads.size() + 1), (count + least - 1) / least);
    auto shared = std::make_shared<std::function<void(std::size_t, std::size_t)>>(std::move(work));

    std::unique_lock lock(m_mutex);
    std::vector<job_t> parts;
    parts.reserve(ranges);
    for (std::size_t i = 0; i < ranges; ++i) {
        // Each range is count / ranges items, and the first few one more, for what's left.
        const std::size_t begin = (i * (count / ranges)) + std::min(i, count % ranges);
        const std::size_t end = begin + (count / ranges) + (i < count % ranges ? 1 : 0);
        parts.push_back(add(lock, [shared, begin, end] { std::invoke(*shared, begin, end); }, after));
    }
    return add(lock, [] {}, ranges == 0 ? after : std::span<const job_t>(parts));
}

bool jobs_t::finished(job_t job) const {
    const std::unique_lock lock(m_mutex);
    return finished(lock, job);
}

void jobs_t::wait(job_t job) {
    std::unique_lock lock(m_mutex);
    while (!finished(lock, job)) {
        if (!m_queue.empty()) {
            run_next(lock);
        } else {
            m_finished.wait(lock);
        }
    }
}

void jobs_t::to_main(std::move_only_function<void()> work) {
    const std::scoped_lock lock(m_main_mutex);
    m_main.push_back(std::move(work));
}

void jobs_t::hand_over() {
    std::vector<std::move_only_function<void()>> ready;
    {
        const std::scoped_lock lock(m_main_mutex);
        ready.swap(m_main);
    }
    for (auto& work : ready) {
        work();
    }
}

int jobs_t::workers() const {
    return static_cast<int>(m_threads.size());
}

job_t jobs_t::add(std::unique_lock<std::mutex>& lock, std::move_only_function<void()> work,
                  std::span<const job_t> after) {
    std::uint32_t slot = 0;
    if (!m_free.empty()) {
        slot = m_free.back();
        m_free.pop_back();
    } else {
        slot = static_cast<std::uint32_t>(m_slots.size());
        m_slots.emplace_back();
    }
    std::uint32_t waiting = 0;
    for (const job_t before : after) {
        if (!finished(lock, before)) {
            m_slots[slot_of(before)].then.push_back(slot);
            ++waiting;
        }
    }
    slot_t& added = m_slots[slot];
    added.waiting = waiting;
    added.work = std::move(work);
    ++m_unfinished;
    if (waiting == 0) {
        m_queue.push_back(slot);
        m_ready.notify_one();
    }
    return (static_cast<job_t>(added.generation) << 32U) | (static_cast<job_t>(slot) + 1);
}

bool jobs_t::finished(const std::unique_lock<std::mutex>& /*lock*/, job_t job) const {
    if (job == 0) {
        return true;
    }
    const std::uint32_t slot = slot_of(job);
    return slot >= m_slots.size() || m_slots[slot].generation != generation_of(job);
}

// It's noexcept so that a job that throws ends the program, rather than leaving its slot unfinished
// for good.
// NOLINTNEXTLINE(bugprone-exception-escape)
void jobs_t::run_next(std::unique_lock<std::mutex>& lock) noexcept {
    const std::uint32_t slot = m_queue.front();
    m_queue.pop_front();
    std::move_only_function<void()> work = std::move(m_slots[slot].work);
    m_slots[slot].work = nullptr;
    lock.unlock();
    work();
    // What the work holds goes before the lock is taken again, since letting it go can start jobs.
    work = nullptr;
    lock.lock();

    slot_t& done = m_slots[slot];
    const std::vector<std::uint32_t> then = std::move(done.then);
    done.then.clear();
    ++done.generation;
    m_free.push_back(slot);
    for (const std::uint32_t next : then) {
        if (--m_slots[next].waiting == 0) {
            m_queue.push_back(next);
            m_ready.notify_one();
        }
    }
    --m_unfinished;
    m_finished.notify_all();
}

void jobs_t::work_loop() {
    std::unique_lock lock(m_mutex);
    while (true) {
        if (!m_queue.empty()) {
            run_next(lock);
        } else if (m_stopping) {
            return;
        } else {
            m_ready.wait(lock);
        }
    }
}

} // namespace fbe
