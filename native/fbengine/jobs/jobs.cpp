// jobs_t: one lock over the jobs, each in a slot that a finished job gives back, a queue of the
// ready ones, and threads that wait for one. A job's dependents are in its slot, so finishing it
// readies them without a search, and so are the jobs it runs after, so a wait finds what it can
// run of its own job and runs nothing else: a wait never holds up its thread with another's work,
// never stacks one wait on another but its own job's, and can't wait for itself through them.

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

fbe::job_t job_of(std::uint32_t slot, std::uint32_t generation) {
    return (static_cast<fbe::job_t>(generation) << 32U) | (static_cast<fbe::job_t>(slot) + 1);
}

} // namespace

namespace fbe {

jobs_t::jobs_t(int workers) {
    const int count = std::max(workers, 1);
    m_threads.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        m_threads.emplace_back([this] { work_loop(); });
    }
    // Each has started before this gives back, since a process that ends while a thread is still
    // registering what it needs with winpthread hangs in its exit.
    std::unique_lock lock(m_mutex);
    m_all_started.wait(lock, [this, count] { return m_started == count; });
}

jobs_t::~jobs_t() {
    // Work handed to the main thread can start jobs, so the two are run in turn until neither has
    // any left, before the workers stop.
    while (true) {
        {
            std::unique_lock lock(m_mutex);
            while (m_unfinished > 0) {
                if (const std::optional<std::uint32_t> slot = next_ready(lock)) {
                    run(lock, *slot);
                } else {
                    m_finished.wait(lock);
                }
            }
        }
        std::vector<work_t> left;
        {
            const std::scoped_lock lock(m_main_mutex);
            left.swap(m_main);
        }
        if (left.empty()) {
            break;
        }
        for (work_t& work : left) {
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

job_t jobs_t::run(work_t work, std::span<const job_t> after) {
    std::unique_lock lock(m_mutex);
    return add(lock, std::move(work), after);
}

job_t jobs_t::run_for(std::size_t count, std::size_t grain,
                      std::function<void(std::size_t begin, std::size_t end)> work,
                      std::span<const job_t> after) {
    // As many ranges as there are grains, so each has a grain at least, and none past the most.
    const std::size_t ranges =
        count == 0 ? 0
                   : std::max<std::size_t>(1, std::min(ranges_a_thread * (m_threads.size() + 1),
                                                       count / std::max<std::size_t>(grain, 1)));
    auto shared = std::make_shared<std::function<void(std::size_t, std::size_t)>>(std::move(work));

    std::unique_lock lock(m_mutex);
    std::vector<job_t> parts;
    parts.reserve(ranges);
    for (std::size_t i = 0; i < ranges; ++i) {
        // Each range is count / ranges items, and the first few one more, for what's left.
        const std::size_t begin = (i * (count / ranges)) + std::min(i, count % ranges);
        const std::size_t end = begin + (count / ranges) + (i < count % ranges ? 1 : 0);
        parts.push_back(
            add(lock, [shared, begin, end] { std::invoke(*shared, begin, end); }, after));
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
        if (const std::optional<std::uint32_t> slot = ready_for(lock, job)) {
            run(lock, *slot);
        } else {
            // What's left of it runs on other threads, or waits for what does.
            m_finished.wait(lock);
        }
    }
}

void jobs_t::to_main(work_t work) {
    const std::scoped_lock lock(m_main_mutex);
    m_main.push_back(std::move(work));
}

void jobs_t::hand_over() {
    std::vector<work_t> ready;
    {
        const std::scoped_lock lock(m_main_mutex);
        ready.swap(m_main);
    }
    for (work_t& work : ready) {
        work();
    }
}

int jobs_t::workers() const {
    return static_cast<int>(m_threads.size());
}

job_t jobs_t::add(std::unique_lock<std::mutex>& lock, work_t work, std::span<const job_t> after) {
    std::uint32_t slot = 0;
    if (!m_free.empty()) {
        slot = m_free.back();
        m_free.pop_back();
    } else {
        slot = static_cast<std::uint32_t>(m_slots.size());
        m_slots.emplace_back();
    }
    std::vector<job_t> waits_for;
    for (const job_t before : after) {
        if (!finished(lock, before)) {
            m_slots[slot_of(before)].then.push_back(slot);
            waits_for.push_back(before);
        }
    }
    slot_t& added = m_slots[slot];
    added.waiting = static_cast<std::uint32_t>(waits_for.size());
    added.after = std::move(waits_for);
    added.work = std::move(work);
    added.state = added.waiting == 0 ? state_t::READY : state_t::WAITING;
    ++m_unfinished;
    const job_t job = job_of(slot, added.generation);
    if (added.state == state_t::READY) {
        m_queue.push_back(job);
        m_ready.notify_one();
    }
    return job;
}

bool jobs_t::finished(const std::unique_lock<std::mutex>& /*lock*/, job_t job) const {
    if (job == 0) {
        return true;
    }
    const std::uint32_t slot = slot_of(job);
    return slot >= m_slots.size() || m_slots[slot].generation != generation_of(job);
}

std::optional<std::uint32_t> jobs_t::next_ready(const std::unique_lock<std::mutex>& lock) {
    while (!m_queue.empty()) {
        const job_t job = m_queue.front();
        m_queue.pop_front();
        if (!finished(lock, job) && m_slots[slot_of(job)].state == state_t::READY) {
            return slot_of(job);
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> jobs_t::ready_for(const std::unique_lock<std::mutex>& lock,
                                               job_t job) {
    // Each slot is looked at once a search, however many of the jobs run after it.
    ++m_searches;
    m_search.clear();
    m_search.push_back(job);
    while (!m_search.empty()) {
        const job_t at = m_search.back();
        m_search.pop_back();
        if (finished(lock, at)) {
            continue;
        }
        slot_t& slot = m_slots[slot_of(at)];
        if (slot.searched == m_searches) {
            continue;
        }
        slot.searched = m_searches;
        if (slot.state == state_t::READY) {
            return slot_of(at);
        }
        if (slot.state == state_t::WAITING) {
            m_search.insert(m_search.end(), slot.after.begin(), slot.after.end());
        }
    }
    return std::nullopt;
}

// It's noexcept so that a job that throws ends the program, rather than leaving its slot unfinished
// for good.
// NOLINTNEXTLINE(bugprone-exception-escape)
void jobs_t::run(std::unique_lock<std::mutex>& lock, std::uint32_t slot) noexcept {
    // A queued job a wait runs is passed over in the queue, since it isn't READY any more.
    m_slots[slot].state = state_t::RUNNING;
    work_t work = std::move(m_slots[slot].work);
    lock.unlock();
    work();
    // What the work holds goes before the lock is taken again, since letting it go can start jobs.
    work = {};
    lock.lock();

    slot_t& done = m_slots[slot];
    const std::vector<std::uint32_t> then = std::move(done.then);
    done.then.clear();
    done.after.clear();
    done.state = state_t::FREE;
    ++done.generation;
    m_free.push_back(slot);
    for (const std::uint32_t next : then) {
        slot_t& waiting = m_slots[next];
        if (--waiting.waiting == 0) {
            waiting.state = state_t::READY;
            m_queue.push_back(job_of(next, waiting.generation));
            m_ready.notify_one();
        }
    }
    --m_unfinished;
    m_finished.notify_all();
}

void jobs_t::work_loop() {
    std::unique_lock lock(m_mutex);
    ++m_started;
    m_all_started.notify_all();
    while (true) {
        if (const std::optional<std::uint32_t> slot = next_ready(lock)) {
            run(lock, *slot);
        } else if (m_stopping) {
            return;
        } else {
            m_ready.wait(lock);
        }
    }
}

} // namespace fbe
