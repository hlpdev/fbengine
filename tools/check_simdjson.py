"""Checks each target that builds gltf.cpp reads the simdjson fastgltf fetched, not one installed: an Intel
Mac's Homebrew puts its own in /usr/local/include, which a dependency's plain -I can reach before the
fetched folder, as that comes as -isystem from fastgltf, a SYSTEM dependency. It runs each target's own
compile of gltf.cpp from compile_commands.json, asking the compiler what it includes instead: -M of GCC
and Clang, /showIncludes of MSVC's cl and clang-cl.

    python tools/check_simdjson.py <build folder> <the simdjson.h the build gives them>

With no compile_commands.json, as the Visual Studio generator writes none, it says so and exits 77,
which ctest takes as skipped.
"""

import json
import pathlib
import shlex
import subprocess
import sys

# What builds gltf.cpp, by the object folder each compiles into.
TARGETS = ["fbengine_native", "fbengine_internal", "fbengine_tools_native", "fbengine_assets_tests",
           "fbengine_tools_tests"]
SKIPPED = 77


def split_command(text, windows):
    """A command line's words: by Windows' own rules on Windows, where a backslash is a path's and
    quotes are dropped, and a POSIX shell's elsewhere."""
    if not windows:
        return shlex.split(text)
    words = []
    word = ""
    quoted = False
    started = False
    at = 0
    while at < len(text):
        char = text[at]
        if char == "\\":
            run = 0
            while at < len(text) and text[at] == "\\":
                run += 1
                at += 1
            if at < len(text) and text[at] == '"':
                word += "\\" * (run // 2)
                if run % 2:
                    word += '"'
                    at += 1
            else:
                word += "\\" * run
            started = True
            continue
        if char == '"':
            quoted = not quoted
            started = True
        elif char in " \t" and not quoted:
            if started:
                words.append(word)
            word = ""
            started = False
        else:
            word += char
            started = True
        at += 1
    if started:
        words.append(word)
    return words


def headers_of(depfile):
    """What a make rule from -M names after its target: its lines joined, and "\\ " a space in a name."""
    text = depfile.replace("\\\r\n", " ").replace("\\\n", " ")
    words = []
    word = ""
    at = 0
    while at < len(text):
        char = text[at]
        if char == "\\" and at + 1 < len(text) and text[at + 1] == " ":
            word += " "
            at += 2
            continue
        if char.isspace():
            if word:
                words.append(word)
            word = ""
        else:
            word += char
        at += 1
    if word:
        words.append(word)
    # The first is the target, ending in a colon.
    while words and not words[0].endswith(":"):
        words.pop(0)
    return words[1:]


def arguments_of(entry, windows):
    if "arguments" in entry:
        return list(entry["arguments"])
    return split_command(entry["command"], windows)


def is_msvc(compiler):
    name = pathlib.PureWindowsPath(compiler).name.lower()
    return name in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")


def includes(entry, windows):
    """The headers the compile reads, as the compiler says, its object and output left out."""
    words = arguments_of(entry, windows)
    if is_msvc(words[0]):
        kept = [word for word in words if not word.lower().startswith(("/fo", "-fo", "/fd", "-fd"))]
        done = subprocess.run(kept + ["/Zs", "/showIncludes"], cwd=entry["directory"],
                              capture_output=True, text=True)
        if done.returncode != 0:
            raise SystemExit(f"its compile failed: {(done.stdout + done.stderr)[:2000]}")
        marker = "including file:"
        return [line.split(marker, 1)[1].strip() for line in done.stdout.splitlines()
                if marker in line.lower()]
    kept = []
    skip = False
    for word in words:
        if skip:
            skip = False
            continue
        if word in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        if word in ("-c", "-MD", "-MMD") or (word.startswith(("-o", "-MF")) and len(word) > 3):
            continue
        kept.append(word)
    done = subprocess.run(kept + ["-M"], cwd=entry["directory"], capture_output=True, text=True)
    if done.returncode != 0:
        raise SystemExit(f"its compile failed: {done.stderr[:2000]}")
    return headers_of(done.stdout)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    build = pathlib.Path(sys.argv[1]).resolve()
    given = pathlib.Path(sys.argv[2]).resolve()
    commands = build / "compile_commands.json"
    if not commands.exists():
        print(f"no {commands.name} in {build}, as the Visual Studio generator writes none: skipped")
        return SKIPPED
    if not given.exists():
        print(f"{given} isn't there")
        return 1
    windows = sys.platform.startswith("win")
    entries = json.loads(commands.read_text(encoding="utf-8"))
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
        found = [pathlib.Path(name) for name in includes(entry, windows)
                 if pathlib.PurePath(name).name == "simdjson.h"]
        resolved = [path.resolve() if path.is_absolute() else (pathlib.Path(entry["directory"]) / path).resolve()
                    for path in found]
        if resolved != [given]:
            print(f"{target} reads {[str(path) for path in resolved]}, not {given}")
            wrong = True
        seen.append(target)
    missing = sorted(set(TARGETS) - set(seen))
    if missing:
        print(f"no compile of gltf.cpp found for {missing}")
        return 1
    if wrong:
        return 1
    print(f"{len(seen)} targets read the simdjson fastgltf fetched: {', '.join(sorted(seen))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
