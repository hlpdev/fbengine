"""Rewrites golden images: runs the golden image tests with FBENGINE_GOLDEN=update, so each writes
the frame it drew over its PNG in tests/golden/, then says which changed. Golden images are
lavapipe's, which ctest's tests use; a test on another GPU refuses to write one.

    python tools/golden.py [--build <folder>] [<ctest -R pattern>]

A changed image is reviewed as code is: look at it before it's committed.
"""

import argparse
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", default="build", help="the build folder (default: build)")
    parser.add_argument("pattern", nargs="?", help="only the golden image tests this matches")
    options = parser.parse_args()
    build = (ROOT / options.build).resolve()

    if subprocess.run(["cmake", "--build", str(build)], cwd=ROOT).returncode != 0:
        sys.exit("the build failed")
    command = ["ctest", "--test-dir", str(build), "-L", "golden", "--output-on-failure"]
    if options.pattern:
        command += ["-R", options.pattern]
    environment = dict(os.environ, SDL_VIDEO_DRIVER="offscreen", FBENGINE_GOLDEN="update")
    if subprocess.run(command, cwd=ROOT, env=environment).returncode != 0:
        sys.exit("a golden image test failed, so not every image was written")

    changed = subprocess.run(["git", "status", "--short", "--", "tests/golden"], cwd=ROOT,
                             capture_output=True, text=True, check=False).stdout
    print()
    print(f"changed golden images:\n{changed}" if changed else "no golden image changed")


if __name__ == "__main__":
    main()
