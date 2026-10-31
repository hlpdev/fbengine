#pragma once

// What fbengine_native's own files share: its errors, and SDL's video, which starts the first
// time something needs it.

#include <SDL3/SDL_events.h>
#include <string_view>

namespace fbe {

/// What an event read by fbe_frame_flip says of the window.
void window_event(const SDL_Event& event);

/// Keeps `message` for fbe_error(), and returns false, for a call that failed.
bool fail(std::string_view message);

/// SDL's last error, for fbe_error(), with what was being done, and returns false.
bool fail_sdl(std::string_view doing);

/// Starts SDL's video, offscreen if fbe_window_set_headless said so; false if it can't.
bool start_video();

/// Whether SDL's video has started, after which headless can't change.
bool video_started();

} // namespace fbe
