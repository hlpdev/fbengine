#pragma once

// What fbengine_native's own files share: its errors, and SDL's video, which starts the first
// time something needs it.

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>
#include <string_view>

namespace fbe {

/// What an event read by fbe_frame_flip says of the window.
void window_event(const SDL_Event& event);

/// The window fbe_window_open opened, or null.
SDL_Window* window();

/// Keeps `message` for fbe_error(), and returns false, for a call that failed.
bool fail(std::string_view message) noexcept;

/// On macOS, a pool of what the system's Objective-C side hands back to be let go of later, as
/// Metal's command buffers are under MoltenVK: what's handed back while one's there goes with it.
/// A thread that no event loop drains keeps them all otherwise, as a program that loads and frees
/// and never Flips does, and a server. One goes round each piece of the engine's work that makes
/// them. Elsewhere it's nothing.
class autoreleased_t {
public:
    autoreleased_t() noexcept;
    ~autoreleased_t();
    autoreleased_t(const autoreleased_t&) = delete;
    autoreleased_t& operator=(const autoreleased_t&) = delete;
    autoreleased_t(autoreleased_t&&) = delete;
    autoreleased_t& operator=(autoreleased_t&&) = delete;

private:
    void* m_pool = nullptr;
};

/// SDL's last error, for fbe_error(), with what was being done, and returns false.
bool fail_sdl(std::string_view doing);

/// Starts SDL's video, offscreen if fbe_window_set_headless said so; false if it can't.
bool start_video();

/// Whether SDL's video has started, after which headless can't change.
bool video_started();

} // namespace fbe
