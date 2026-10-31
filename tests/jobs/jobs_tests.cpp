// The job system: every job runs once, after the jobs it runs after, however many there are and
// whatever threads start and wait for them, and none is lost when the jobs end with work queued.
// Catch2's checks are the test's thread's, so jobs count what they find for it to check.

#include "jobs/jobs.hpp"

#include <array>
#include <atomic>
#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

// How many times a job ran, and when it started and finished, by one clock all jobs share, so a
// job that started before one it runs after had finished shows.
struct record_t {
    std::atomic<int> runs = 0;
    std::atomic<std::uint64_t> started = 0;
    std::atomic<std::uint64_t> finished = 0;
};

struct records_t {
    explicit records_t(std::size_t count) : jobs(count) {}

    void run(std::size_t index) {
        record_t& record = jobs[index];
        record.started = ++clock;
        ++record.runs;
        record.finished = ++clock;
    }

    std::vector<record_t> jobs;
    std::atomic<std::uint64_t> clock = 0;
};

} // namespace

TEST_CASE("every job runs once", "[jobs]") {
    for (const int workers : { 1, 3, 16 }) {
        fbe::jobs_t jobs(workers);
        REQUIRE(jobs.workers() == workers);
        constexpr std::size_t count = 20'000;
        records_t records(count);
        std::vector<fbe::job_t> started;
        started.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            started.push_back(jobs.run([&records, i] { records.run(i); }));
        }
        for (const fbe::job_t job : started) {
            jobs.wait(job);
            REQUIRE(jobs.finished(job));
        }
        for (const record_t& record : records.jobs) {
            REQUIRE(record.runs == 1);
        }
    }
}

TEST_CASE("there's a worker at least", "[jobs]") {
    fbe::jobs_t jobs(0);
    REQUIRE(jobs.workers() == 1);
    std::atomic<int> ran = 0;
    jobs.wait(jobs.run([&ran] { ++ran; }));
    REQUIRE(ran == 1);
}

TEST_CASE("a job runs after the jobs it runs after", "[jobs]") {
    fbe::jobs_t jobs(8);
    records_t records(5);

    SECTION("in a chain") {
        fbe::job_t last = 0;
        for (std::size_t i = 0; i < 5; ++i) {
            const std::array before = { last };
            last = jobs.run([&records, i] { records.run(i); }, before);
        }
        jobs.wait(last);
        for (std::size_t i = 1; i < 5; ++i) {
            REQUIRE(records.jobs[i - 1].finished < records.jobs[i].started);
        }
    }

    SECTION("in a diamond") {
        const fbe::job_t top = jobs.run([&records] {
            std::this_thread::sleep_for(5ms);
            records.run(0);
        });
        const std::array after_top = { top };
        const fbe::job_t left = jobs.run([&records] { records.run(1); }, after_top);
        const fbe::job_t right = jobs.run([&records] { records.run(2); }, after_top);
        const std::array sides = { left, right, left };
        jobs.wait(jobs.run([&records] { records.run(3); }, sides));
        REQUIRE(records.jobs[0].finished < records.jobs[1].started);
        REQUIRE(records.jobs[0].finished < records.jobs[2].started);
        REQUIRE(records.jobs[1].finished < records.jobs[3].started);
        REQUIRE(records.jobs[2].finished < records.jobs[3].started);
        REQUIRE(records.jobs[3].runs == 1);
    }

    SECTION("finished, or none, which it doesn't wait for") {
        const fbe::job_t done = jobs.run([&records] { records.run(0); });
        jobs.wait(done);
        const std::array<fbe::job_t, 2> before = { done, 0 };
        jobs.wait(jobs.run([&records] { records.run(1); }, before));
        REQUIRE(records.jobs[1].runs == 1);
        REQUIRE(jobs.finished(0));
    }
}

TEST_CASE("many jobs wait for one", "[jobs]") {
    fbe::jobs_t jobs(8);
    std::atomic<bool> open = false;
    std::atomic<int> early = 0;
    std::atomic<int> ran = 0;
    std::mutex gate_mutex;
    std::unique_lock gate_lock(gate_mutex);
    const fbe::job_t gate = jobs.run([&] {
        const std::scoped_lock wait_for_the_test(gate_mutex);
        open = true;
    });
    const std::array after_gate = { gate };
    std::vector<fbe::job_t> waiting;
    waiting.reserve(2'000);
    for (int i = 0; i < 2'000; ++i) {
        waiting.push_back(jobs.run(
            [&] {
                if (!open) {
                    ++early;
                }
                ++ran;
            },
            after_gate));
    }
    std::this_thread::sleep_for(20ms);
    REQUIRE(ran == 0);
    REQUIRE_FALSE(jobs.finished(waiting.front()));
    gate_lock.unlock();
    for (const fbe::job_t job : waiting) {
        jobs.wait(job);
    }
    REQUIRE(ran == 2'000);
    REQUIRE(early == 0);
}

TEST_CASE("a finished job's handle stays finished when its slot runs another", "[jobs]") {
    fbe::jobs_t jobs(2);
    const fbe::job_t first = jobs.run([] {});
    jobs.wait(first);
    std::mutex hold;
    std::unique_lock held(hold);
    std::atomic<bool> running = false;
    const fbe::job_t second = jobs.run([&hold, &running] {
        running = true;
        const std::scoped_lock wait(hold);
    });
    // It's on a worker before this thread waits, which could otherwise run it, and wait for itself.
    while (!running) {
        std::this_thread::yield();
    }
    // The second has the first's slot, which a finished job gives back.
    REQUIRE((second & 0xFFFF'FFFFU) == (first & 0xFFFF'FFFFU));
    REQUIRE(second != first);
    REQUIRE(jobs.finished(first));
    REQUIRE_FALSE(jobs.finished(second));
    const std::array after_first = { first };
    std::atomic<int> ran = 0;
    jobs.wait(jobs.run([&ran] { ++ran; }, after_first));
    REQUIRE(ran == 1);
    held.unlock();
    jobs.wait(second);
}

TEST_CASE("run_for covers every item once, a grain a range at least", "[jobs]") {
    fbe::jobs_t jobs(6);
    const std::array<std::size_t, 7> counts = { 0, 1, 7, 10, 64, 1'000, 100'003 };
    const std::array<std::size_t, 8> grains = {
        0, 1, 4, 10, 4'096, 1'000'000, SIZE_MAX - 1, SIZE_MAX,
    };
    for (const std::size_t count : counts) {
        for (const std::size_t grain : grains) {
            std::vector<std::atomic<int>> seen(count);
            std::atomic<int> ranges = 0;
            std::atomic<int> empty = 0;
            std::atomic<std::size_t> smallest = SIZE_MAX;
            jobs.wait(jobs.run_for(count, grain, [&](std::size_t begin, std::size_t end) {
                if (begin >= end) {
                    ++empty;
                }
                ++ranges;
                std::size_t least = smallest;
                while (end - begin < least && !smallest.compare_exchange_weak(least, end - begin)) {
                }
                for (std::size_t i = begin; i < end; ++i) {
                    ++seen[i];
                }
            }));
            REQUIRE(empty == 0);
            for (const std::atomic<int>& item : seen) {
                REQUIRE(item == 1);
            }
            // A grain a range at least, or one range for fewer, and at most four a thread.
            if (count == 0) {
                REQUIRE(ranges == 0);
            } else if (count <= std::max<std::size_t>(grain, 1)) {
                REQUIRE(ranges == 1);
            } else {
                REQUIRE(smallest >= std::max<std::size_t>(grain, 1));
            }
            REQUIRE(ranges <= 4 * (jobs.workers() + 1));
        }
    }
}

TEST_CASE("run_for waits for the jobs it runs after", "[jobs]") {
    fbe::jobs_t jobs(4);
    std::atomic<bool> first = false;
    std::atomic<int> early = 0;
    const fbe::job_t slow = jobs.run([&first] {
        std::this_thread::sleep_for(10ms);
        first = true;
    });
    const std::array before = { slow };
    const fbe::job_t all = jobs.run_for(
        1'000, 10,
        [&](std::size_t begin, std::size_t end) {
            if (!first) {
                early += static_cast<int>(end - begin);
            }
        },
        before);
    jobs.wait(all);
    REQUIRE(early == 0);

    // With nothing to do, it waits for them still.
    first = false;
    const fbe::job_t slow_again = jobs.run([&first] {
        std::this_thread::sleep_for(10ms);
        first = true;
    });
    const std::array again = { slow_again };
    jobs.wait(jobs.run_for(0, 1, [](std::size_t, std::size_t) {}, again));
    REQUIRE(first);
}

TEST_CASE("a job can wait for the jobs it starts, with one worker", "[jobs]") {
    fbe::jobs_t jobs(1);
    std::atomic<int> leaves = 0;
    // Three levels of jobs waiting for their children: with one worker, only waits that run the
    // ready jobs themselves get through.
    std::function<void(int)> grow = [&](int depth) {
        if (depth == 0) {
            ++leaves;
            return;
        }
        std::vector<fbe::job_t> children;
        children.reserve(4);
        for (int i = 0; i < 4; ++i) {
            children.push_back(jobs.run([&grow, depth] { grow(depth - 1); }));
        }
        for (const fbe::job_t child : children) {
            jobs.wait(child);
        }
    };
    jobs.wait(jobs.run([&grow] { grow(3); }));
    REQUIRE(leaves == 64);
}

TEST_CASE("work handed to the main thread runs at hand_over, in order", "[jobs]") {
    fbe::jobs_t jobs(4);
    std::vector<int> order;
    std::atomic<int> outside = 0;
    const std::thread::id main = std::this_thread::get_id();
    std::vector<fbe::job_t> started;
    started.reserve(100);
    for (int i = 0; i < 100; ++i) {
        started.push_back(jobs.run([&jobs, &order, &outside, main, i] {
            jobs.to_main([&order, &outside, main, i] {
                if (std::this_thread::get_id() != main) {
                    ++outside;
                }
                order.push_back(i);
            });
        }));
    }
    for (const fbe::job_t job : started) {
        jobs.wait(job);
    }
    REQUIRE(order.empty());
    jobs.hand_over();
    REQUIRE(order.size() == 100);
    REQUIRE(outside == 0);

    // What a hand-over queues waits for the next.
    order.clear();
    jobs.to_main([&jobs, &order] {
        order.push_back(1);
        jobs.to_main([&order] { order.push_back(2); });
    });
    jobs.hand_over();
    REQUIRE(order == std::vector{ 1 });
    jobs.hand_over();
    REQUIRE(order == std::vector{ 1, 2 });
    jobs.hand_over();
    REQUIRE(order == std::vector{ 1, 2 });
}

TEST_CASE("ending the jobs with work queued runs all of it once", "[jobs]") {
    for (int round = 0; round < 20; ++round) {
        std::atomic<int> ran = 0;
        std::atomic<int> handed = 0;
        std::atomic<int> late = 0;
        {
            fbe::jobs_t jobs((round % 4) + 1);
            fbe::job_t last = 0;
            for (int i = 0; i < 500; ++i) {
                const std::array before = { last };
                const fbe::job_t job = jobs.run(
                    [&jobs, &ran, &handed, &late] {
                        ++ran;
                        // A job that starts another, and hands work over that starts one more.
                        jobs.run([&ran] { ++ran; });
                        jobs.to_main([&jobs, &handed, &late] {
                            ++handed;
                            jobs.run([&late] { ++late; });
                        });
                    },
                    i % 3 == 0 ? std::span<const fbe::job_t>(before)
                               : std::span<const fbe::job_t>());
                last = job;
            }
            jobs.run_for(1'000, 7, [&ran](std::size_t begin, std::size_t end) {
                ran += static_cast<int>(end - begin);
            });
        }
        REQUIRE(ran == (500 * 2) + 1'000);
        REQUIRE(handed == 500);
        REQUIRE(late == 500);
    }
}

TEST_CASE("under stress, from many threads, every job runs once and in order", "[jobs][stress]") {
    // Random graphs of jobs, each after a few of those made before it, started and waited for by
    // several threads at once, some jobs starting and waiting for jobs of their own. Catch2's seed
    // makes them, so `--rng-seed` makes a run's again.
    std::mt19937 random(Catch::getSeed());
    for (int round = 0; round < 12; ++round) {
        const int workers = std::array{ 1, 2, 5, 31 }[static_cast<std::size_t>(round % 4)];
        fbe::jobs_t jobs(workers);
        constexpr int threads = 4;
        constexpr std::size_t each = 3'000;
        records_t records(threads * each);
        std::vector<std::vector<fbe::job_t>> handles(threads);
        std::vector<std::vector<std::vector<std::size_t>>> afters(threads);
        std::atomic<int> children = 0;
        std::atomic<int> nested = 0;
        std::vector<std::uint32_t> seeds;
        seeds.reserve(threads);
        for (int t = 0; t < threads; ++t) {
            seeds.push_back(static_cast<std::uint32_t>(random()));
        }

        std::vector<std::thread> starters;
        starters.reserve(threads);
        for (int t = 0; t < threads; ++t) {
            starters.emplace_back([&, t] {
                std::mt19937 own(seeds[static_cast<std::size_t>(t)]);
                std::vector<fbe::job_t>& made = handles[static_cast<std::size_t>(t)];
                std::vector<std::vector<std::size_t>>& after = afters[static_cast<std::size_t>(t)];
                for (std::size_t i = 0; i < each; ++i) {
                    const std::size_t index = (static_cast<std::size_t>(t) * each) + i;
                    std::vector<fbe::job_t> before;
                    std::vector<std::size_t> before_indices;
                    const std::size_t count = i == 0 ? 0 : own() % 4;
                    for (std::size_t k = 0; k < count; ++k) {
                        const std::size_t pick = own() % i;
                        before.push_back(made[pick]);
                        before_indices.push_back((static_cast<std::size_t>(t) * each) + pick);
                    }
                    const bool nests = own() % 50 == 0;
                    if (nests) {
                        ++nested;
                    }
                    made.push_back(jobs.run(
                        [&records, &jobs, &children, index, nests] {
                            if (nests) {
                                const fbe::job_t child = jobs.run([&children] { ++children; });
                                jobs.wait(child);
                            }
                            records.run(index);
                        },
                        before));
                    after.push_back(std::move(before_indices));
                    if (own() % 100 == 0) {
                        jobs.wait(made[own() % made.size()]);
                    }
                }
                for (const fbe::job_t job : made) {
                    jobs.wait(job);
                }
            });
        }
        for (std::thread& starter : starters) {
            starter.join();
        }

        REQUIRE(children == nested);
        for (int t = 0; t < threads; ++t) {
            for (std::size_t i = 0; i < each; ++i) {
                const std::size_t index = (static_cast<std::size_t>(t) * each) + i;
                const record_t& record = records.jobs[index];
                REQUIRE(record.runs == 1);
                for (const std::size_t before : afters[static_cast<std::size_t>(t)][i]) {
                    REQUIRE(records.jobs[before].finished < record.started);
                }
            }
        }
    }
}

TEST_CASE("a job can hold what can't be copied", "[jobs]") {
    fbe::jobs_t jobs(2);
    auto owned = std::make_unique<int>(41);
    std::atomic<int> seen = 0;
    jobs.wait(jobs.run([owned = std::move(owned), &seen, &jobs] mutable {
        ++*owned;
        jobs.to_main([owned = std::move(owned), &seen] { seen = *owned; });
    }));
    REQUIRE(seen == 0);
    jobs.hand_over();
    REQUIRE(seen == 42);
    REQUIRE_FALSE(fbe::work_t());
}

namespace {

// How deep in waits the running job is, on its thread.
thread_local int nesting = 0;

// Holds a worker until it's let go, so the jobs queued meanwhile are in a known order.
struct hold_t {
    explicit hold_t(fbe::jobs_t& jobs) : held(mutex) {
        job = jobs.run([this] {
            holding = true;
            const std::scoped_lock wait(mutex);
        });
        while (!holding) {
            std::this_thread::yield();
        }
    }

    std::mutex mutex;
    std::unique_lock<std::mutex> held;
    std::atomic<bool> holding = false;
    fbe::job_t job = 0;
};

// Waits up to a minute for `job`, for a slow machine, without running it here.
bool finishes(const fbe::jobs_t& jobs, fbe::job_t job) {
    for (int i = 0; i < 6'000 && !jobs.finished(job); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    return jobs.finished(job);
}

} // namespace

TEST_CASE("a wait runs only its job and what it runs after", "[jobs]") {
    // One worker: A waits for B, and C for D, which runs after A. A wait that ran whatever was
    // first in the queue would run C inside A's, and C would wait for D, which waits for A.
    // Leaked, so a deadlock fails the test rather than hanging it.
    auto* jobs = new fbe::jobs_t(1);
    hold_t hold(*jobs);
    std::atomic<fbe::job_t> b = 0;
    std::atomic<fbe::job_t> d = 0;
    const fbe::job_t a = jobs->run([&] { jobs->wait(b.load()); });
    const fbe::job_t c = jobs->run([&] { jobs->wait(d.load()); });
    b = jobs->run([] {});
    const std::array after_a = { a };
    d = jobs->run([] {}, after_a);
    hold.held.unlock();
    REQUIRE(finishes(*jobs, d));
    REQUIRE(jobs->finished(a));
    REQUIRE(jobs->finished(c));
    delete jobs;
}

TEST_CASE("a wait ends soon after its job, whatever else is queued", "[jobs]") {
    using steady_t = std::chrono::steady_clock;
    fbe::jobs_t jobs(2);
    std::atomic<int> started = 0;
    std::atomic<bool> long_ran_here = false;
    const std::thread::id here = std::this_thread::get_id();
    // Both workers busy, one with a long load and one with what the frame's job runs after.
    jobs.run([&] {
        ++started;
        std::this_thread::sleep_for(3s);
    });
    const fbe::job_t before = jobs.run([&] {
        ++started;
        std::this_thread::sleep_for(20ms);
    });
    while (started < 2) {
        std::this_thread::yield();
    }
    // Another long load is queued first; a wait that ran it would take three seconds.
    jobs.run([&] {
        long_ran_here = std::this_thread::get_id() == here;
        std::this_thread::sleep_for(3s);
    });
    const std::array after = { before };
    const fbe::job_t frame = jobs.run([] {}, after);
    const auto start = steady_t::now();
    jobs.wait(frame);
    REQUIRE(steady_t::now() - start < 1500ms);
    REQUIRE_FALSE(long_ran_here);
}

TEST_CASE("waits don't stack on a thread", "[jobs]") {
    fbe::jobs_t jobs(2);
    hold_t slow(jobs);
    std::atomic<int> deepest = 0;
    std::atomic<int> ran = 0;
    // Twenty thousand jobs each waiting for one that's held: each worker takes one and waits, and
    // never runs another inside its wait, as it would run every one, a few frames deeper each.
    for (int i = 0; i < 20'000; ++i) {
        jobs.run([&] {
            const int depth = ++nesting;
            int most = deepest;
            while (depth > most && !deepest.compare_exchange_weak(most, depth)) {}
            jobs.wait(slow.job);
            --nesting;
            ++ran;
        });
    }
    std::this_thread::sleep_for(100ms);
    slow.held.unlock();
    jobs.wait(slow.job);
    while (ran < 20'000) {
        std::this_thread::yield();
    }
    REQUIRE(deepest == 1);
}

TEST_CASE("a wait for a long chain runs it without going deeper", "[jobs]") {
    fbe::jobs_t jobs(3);
    std::atomic<int> ran = 0;
    fbe::job_t last = 0;
    for (int i = 0; i < 5'000; ++i) {
        const std::array before = { last };
        last = jobs.run([&ran] { ++ran; }, before);
    }
    jobs.wait(last);
    REQUIRE(ran == 5'000);

    // And a lattice, where every job runs after all of the layer before, looked at once a search.
    std::vector<fbe::job_t> layer;
    for (int depth = 0; depth < 30; ++depth) {
        std::vector<fbe::job_t> next;
        next.reserve(8);
        for (int i = 0; i < 8; ++i) {
            next.push_back(jobs.run([&ran] { ++ran; }, layer));
        }
        layer = std::move(next);
    }
    jobs.wait(jobs.run([] {}, layer));
    REQUIRE(ran == 5'000 + (30 * 8));
}
