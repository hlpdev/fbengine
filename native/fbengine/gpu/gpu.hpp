#pragma once

// What the window and the frame call of the GPU: the window's coming and going, which its
// swapchain follows, and Flip's present.

struct SDL_Window;

namespace fbe {

/// The window `opened` is open: one `on_screen` starts the GPU, which it needs to be seen, and a
/// started GPU makes it a swapchain; false, with fbe_error's why, if that can't be done.
bool gpu_window_opened(SDL_Window* opened, bool on_screen);

/// The window is closing: its swapchain and surface go first.
void gpu_window_closing();

/// Presents the frame in the window, waiting for the display if `wait`, unless there's no window
/// or swapchain, the GPU hasn't started, or the window's minimized.
void gpu_present(bool wait);

} // namespace fbe
