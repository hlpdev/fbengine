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

TEST_CASE("run_for covers every item once", "[jobs]") {
    fbe::jobs_t jobs(6);
    const std::array<std::size_t, 6> counts = { 0, 1, 7, 64, 1'000, 100'003 };
    const std::array<std::size_t, 5> grains = { 0, 1, 10, 4'096, 1'000'000 };
    for (const std::size_t count : counts) {
        for (const std::size_t grain : grains) {
            std::vector<std::atomic<int>> seen(count);
            std::atomic<int> ranges = 0;
            std::atomic<int> empty = 0;
            jobs.wait(jobs.run_for(count, grain, [&](std::size_t begin, std::size_t end) {
                if (begin >= end) {
                    ++empty;
                }
                ++ranges;
                for (std::size_t i = begin; i < end; ++i) {
                    ++seen[i];
                }
            }));
            REQUIRE(empty == 0);
            for (const std::atomic<int>& item : seen) {
                REQUIRE(item == 1);
            }
            // At least a grain a range, and at most four ranges a thread.
            const std::size_t least = grain == 0 ? 1 : grain;
            REQUIRE(static_cast<std::size_t>(ranges) <= (count + least - 1) / least);
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
