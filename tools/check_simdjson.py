"""Checks each target that builds gltf.cpp reads the simdjson fastgltf fetched, not one installed: an Intel
Mac's Homebrew puts its own in /usr/local/include, which a dependency's plain -I can reach before the
fetched folder, as that comes as -isystem from fastgltf, a SYSTEM dependency. It runs each target's own
compile of gltf.cpp from compile_commands.json, asking the compiler what it includes instead.

    python tools/check_simdjson.py <build folder>
"""

import json
import pathlib
import shlex
import subprocess
import sys

# What builds gltf.cpp, by the object folder each compiles into.
TARGETS = ["fbengine_native", "fbengine_internal", "fbengine_tools_native", "fbengine_assets_tests",
           "fbengine_tools_tests"]


def arguments_of(entry):
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"], posix=not sys.platform.startswith("win"))


def includes(entry):
    """The headers the compile reads, by the compiler's own -M, its object and output left out."""
    words = arguments_of(entry)
    kept = []
    skip = False
    for word in words:
        if skip:
            skip = False
            continue
        if word in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        if word in ("-c", "-MD", "-MMD") or word.startswith(("-o", "-MF")) and len(word) > 3:
            continue
        kept.append(word)
    done = subprocess.run(kept + ["-M"], cwd=entry["directory"], capture_output=True, text=True)
    if done.returncode != 0:
        raise SystemExit(f"its compile failed: {done.stderr[:2000]}")
    return done.stdout.replace("\\\n", " ").split()


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    build = pathlib.Path(sys.argv[1]).resolve()
    fetched = next(build.glob("_deps/fastgltf-src/deps/simdjson/simdjson.h"), None)
    if fetched is None:
        print("fastgltf's simdjson.h isn't in the build")
        return 1
    entries = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    wrong = False
    seen = []
    for entry in entries:
        file = pathlib.PurePath(entry["file"]).as_posix()
        if not file.endswith("native/fbengine/assets/gltf.cpp"):
            continue
        output = pathlib.PurePath(entry.get("output", "")).as_posix()
        target = next((name for name in TARGETS if f"/{name}.dir/" in "/" + output), None)
        if target is None:
            continue
        if any(word in ("cl", "cl.exe") or word.endswith(("cl.exe", "/cl")) for word in arguments_of(entry)[:1]):
            print(f"{target}: MSVC's compile, which this can't ask")
            continue
        found = [pathlib.Path(name) for name in includes(entry) if pathlib.PurePath(name).name == "simdjson.h"]
        resolved = [path.resolve() if path.is_absolute() else (pathlib.Path(entry["directory"]) / path).resolve()
                    for path in found]
        if resolved != [fetched.resolve()]:
            print(f"{target} reads {[str(path) for path in resolved]}, not {fetched}")
            wrong = True
        seen.append(target)
    missing = sorted(set(TARGETS) - set(seen))
    # The tests' targets are only there in a build with them.
    if any(name in missing for name in ("fbengine_native", "fbengine_tools_native")):
        print(f"no compile of gltf.cpp found for {missing}")
        return 1
    if wrong:
        return 1
    print(f"{len(seen)} targets read the simdjson fastgltf fetched: {', '.join(sorted(seen))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
