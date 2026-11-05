"""Checks check-exports.sh's macOS side without a Mac: otool and nm are stubs on PATH, each saying what
a dylib's load commands and symbols would be. A dylib's own install name isn't what it loads, and
Homebrew's simdjson is.

    python tools/check_exports_test.py
"""

import os
import pathlib
import subprocess
import sys
import tempfile

TOOLS = pathlib.Path(__file__).resolve().parent

# What `otool -l` prints of a load command: its kind, then the name it carries.
COMMAND = """Load command {index}
          cmd {kind}
      cmdsize 56
         name {name} (offset 24)
"""

NM = """#!/bin/sh
echo "0000000000001000 T _fbe_hello"
"""

OTOOL = """#!/bin/sh
cat "$(dirname "$0")/loads.txt"
"""


def checked(loads):
    """What check-exports.sh says of a dylib whose load commands are `loads`, and its exit code."""
    with tempfile.TemporaryDirectory() as folder:
        stubs = pathlib.Path(folder)
        for name, text in (("nm", NM), ("otool", OTOOL)):
            (stubs / name).write_text(text, newline="\n")
            (stubs / name).chmod(0o755)
        (stubs / "loads.txt").write_text(
            "".join(COMMAND.format(index=at, kind=kind, name=name)
                    for at, (kind, name) in enumerate(loads)), newline="\n")
        dylib = stubs / "libfbengine_hello_native.dylib"
        dylib.write_bytes(b"")
        environment = dict(os.environ)
        environment["PATH"] = str(stubs) + os.pathsep + environment["PATH"]
        done = subprocess.run(["sh", str(TOOLS / "check-exports.sh"), "fbe_", dylib.as_posix()],
                              capture_output=True, text=True, env=environment)
        return done.returncode, done.stdout + done.stderr


def main():
    own = ("LC_ID_DYLIB", "@rpath/libfbengine_hello_native.dylib")
    system = [("LC_LOAD_DYLIB", "/usr/lib/libSystem.B.dylib"),
              ("LC_LOAD_DYLIB", "/usr/lib/libc++.1.dylib"),
              ("LC_LOAD_DYLIB", "/System/Library/Frameworks/CoreAudio.framework/Versions/A/CoreAudio")]
    code, said = checked([own] + system)
    assert code == 0, f"its own install name and the system's libraries aren't foreign: {said}"
    assert "loads only the system's libraries" in said, said
    code, said = checked([own] + system + [("LC_LOAD_DYLIB", "/usr/local/opt/simdjson/lib/libsimdjson.dylib")])
    assert code != 0, f"Homebrew's simdjson is foreign: {said}"
    assert "/usr/local/opt/simdjson/lib/libsimdjson.dylib" in said, said
    assert "@rpath" not in said, f"only what's foreign is named: {said}"
    code, said = checked([own] + [("LC_LOAD_WEAK_DYLIB", "/opt/homebrew/lib/libz.1.dylib")])
    assert code != 0 and "/opt/homebrew/lib/libz.1.dylib" in said, f"a weak load counts too: {said}"
    print("check-exports.sh's macOS side holds")


if __name__ == "__main__":
    main()
