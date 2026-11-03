#pragma once

// What a thread keeps between jobs, so a job doesn't allocate again.

namespace fbe::scene {

/// What a thread's `thread_local Item* kept = nullptr` points to, made the first time it's asked
/// for and never freed, as the thread keeps it to its end. A thread_local with a destructor is what
/// not to use on a job's thread: first touched there, it registers with winpthread's key, and that
/// hung a program's exit with its workers still running.
template <typename Item>
Item& kept_by_thread(Item*& kept) {
    if (kept == nullptr) {
        kept = new Item();
    }
    return *kept;
}

} // namespace fbe::scene
