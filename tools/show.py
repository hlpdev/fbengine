"""Plays what the engine draws in real windows, for a person to watch; no test runs it.

    python tools/show.py [--seconds <n>] [--list] [<example> ...]

Each example's `--watch` plays by itself, one at a time, until it's stopped after its seconds.
Close its window or press Escape to go on sooner. With no examples named, it's every one under
examples/ that has one, which --list prints, opening nothing.
"""

import argparse
import os
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
# Under examples/, by its way there, since the editor's a package.
EDITOR = "../packages/fbengine_editor"


def watched():
    """The examples with a `--watch`, whose main.fb asks for it, in their folders' order."""
    examples = ROOT / "examples"
    return [main.parent.relative_to(examples).as_posix() for main in sorted(examples.rglob("main.fb"))
            if '"--watch"' in main.read_text(encoding="utf-8")]


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
    parser.add_argument("--list", action="store_true", help="print which it would play, and play none")
    parser.add_argument("examples", nargs="*", help="which to play (default: all)")
    options = parser.parse_args()
    examples = options.examples or watched() + [EDITOR]
    if options.list:
        print("\n".join(examples))
        return

    environment = {name: value for name, value in os.environ.items() if name != "SDL_VIDEO_DRIVER"}
    for example in examples:
        print(f"== {example}", flush=True)
        process = subprocess.Popen(["fb", "run", str(ROOT / "examples" / example), "--", "--watch"], env=environment)
        try:
            process.wait(timeout=options.seconds)
        except subprocess.TimeoutExpired:
            stop(process)


if __name__ == "__main__":
    main()
