"""Checks miniaudio's decoders are compiled to wrap a signed int's overflow, as a corrupt file's samples
and coefficients overflow dr_flac's prediction, which is undefined otherwise: fbengine_miniaudio's
compile of its miniaudio.c, in compile_commands.json, has -fwrapv. tests/audio/fuzz_tests.cpp found
it under UBSan, which no CI job runs, so this keeps it.

    python tools/check_miniaudio_wraps.py <build folder>

With no compile_commands.json, as the Visual Studio generator writes none, or with MSVC, which has no
-fwrapv and doesn't build the engine's DLLs, it says so and exits 77, which ctest takes as skipped.
"""

import json
import pathlib
import sys

SKIPPED = 77


def main():
    build = pathlib.Path(sys.argv[1])
    commands = build / "compile_commands.json"
    if not commands.exists():
        print(f"no {commands}, so nothing to check")
        return SKIPPED
    entries = [entry for entry in json.loads(commands.read_text(encoding="utf-8"))
               if pathlib.PurePath(entry["file"]).name == "miniaudio.c"
               and "fbengine_miniaudio" in entry.get("output", entry["command"] if "command" in entry
                                                      else " ".join(entry.get("arguments", [])))]
    if not entries:
        print("no compile of fbengine_miniaudio's miniaudio.c in compile_commands.json")
        return 1
    failed = False
    for entry in entries:
        words = entry["arguments"] if "arguments" in entry else entry["command"].split()
        compiler = pathlib.PurePath(words[0]).name.lower()
        if compiler.startswith("cl") and not compiler.startswith("clang"):
            print("MSVC builds it, which has no -fwrapv")
            return SKIPPED
        if "-fwrapv" not in words:
            print(f"{entry['file']} is compiled without -fwrapv")
            failed = True
    if failed:
        return 1
    print("miniaudio's decoders wrap a signed int's overflow")
    return 0


if __name__ == "__main__":
    sys.exit(main())
