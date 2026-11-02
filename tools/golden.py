"""Rewrites golden images: runs the golden image tests with FBENGINE_GOLDEN=update, so each test
writes the frames it drew over their PNGs in tests/golden/ once it passes, then says which changed,
and which tests failed, whose images it left. Golden images are lavapipe's, which ctest's tests
use; a test on another GPU refuses to write one.

    python tools/golden.py [--build <folder>] [<ctest -R pattern>]

A changed image is reviewed as code is: look at it before it's committed.
"""

import argparse
import os
import pathlib
import re
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
    # A pattern that matches no test is an error, not an update of nothing.
    command = ["ctest", "--test-dir", str(build), "-L", "golden", "--no-tests=error",
               "--output-on-failure"]
    if options.pattern:
        command += ["-R", options.pattern]
    environment = dict(os.environ, SDL_VIDEO_DRIVER="offscreen", FBENGINE_GOLDEN="update")
    run = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True, text=True,
                         check=False)
    print(run.stdout + run.stderr)
    failed = re.findall(r"^\s*\d+ - (.+?) \((?:Failed|Not Run|Timeout|SEGFAULT|Exception)\)",
                        run.stdout, re.MULTILINE)

    changed = subprocess.run(["git", "status", "--short", "--", "tests/golden/*.png"], cwd=ROOT,
                             capture_output=True, text=True, check=False).stdout
    print(f"changed golden images:\n{changed}" if changed else "no golden image changed")
    if "No tests were found" in run.stdout + run.stderr:
        sys.exit(f"no golden image test matches {options.pattern}")
    if run.returncode != 0:
        names = "".join(f"  {name}\n" for name in failed)
        sys.exit(f"these failed, so their images weren't written:\n{names}" if failed
                 else "ctest failed, and wrote nothing it didn't pass")


if __name__ == "__main__":
    main()
