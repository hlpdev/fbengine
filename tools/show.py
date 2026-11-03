"""Plays what the engine draws in real windows, for a person to watch; no test runs it.

    python tools/show.py [--seconds <n>] [<example> ...]

Each example's `--watch` plays by itself, one at a time, until it's stopped after its seconds.
Close its window or press Escape to go on sooner.
"""

import argparse
import os
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
# The examples with a --watch.
WATCHED = ["arcade", "menu", "controls", "proof/frame_timing_and_input",
           "proof/sprites_over_a_scene", "proof/pixels_of_images_and_textures",
           "proof/display_modes", "proof/text_input", "proof/debug_ui_and_console",
           "proof/in_view_by_bounds", "proof/swept_collisions", "proof/picks", "proof/scene_trees",
           "proof/meshes_in_code", "proof/particles_on_sprites", "proof/pathfinding_over_a_graph",
           "proof/binary_saves_and_maps", "proof/fixed_function_look",
           "proof/cameras_and_viewports", "proof/camera_into_a_texture", "proof/bump",
           "proof/render_settings", "proof/gamma", "proof/streamed_and_positional_sound",
           "proof/seeded_random", "proof/ini_settings", "proof/procedural_map", "proof/room_files",
           "cooked_game", "proof/textures_by_their_settings"]


def stop(process):
    """Stops `process` and what it started, and nothing else."""
    if os.name == "nt":
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(process.pid)], capture_output=True, check=False)
    else:
        process.terminate()
    process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--seconds", type=float, default=20, help="how long each plays (default: 20)")
    parser.add_argument("examples", nargs="*", default=WATCHED, help="which to play (default: all)")
    options = parser.parse_args()

    environment = {name: value for name, value in os.environ.items() if name != "SDL_VIDEO_DRIVER"}
    for example in options.examples:
        print(f"== {example}", flush=True)
        process = subprocess.Popen(["fb", "run", str(ROOT / "examples" / example), "--", "--watch"], env=environment)
        try:
            process.wait(timeout=options.seconds)
        except subprocess.TimeoutExpired:
            stop(process)


if __name__ == "__main__":
    main()
