"""Checks each package's notices name every third-party library its DLL has in it, as the build
made it: CMake's file API, which tests/CMakeLists.txt asks for, gives each DLL's targets, what they
link through every step, and their sources and include folders. Each library it links, each
FetchContent folder its code is compiled from or includes, and each vendored file one of its own
sources #includes has the notices its code asks for in packages/<package>/notices/, which the
package's README names. A library this doesn't know stops it, so a new one gets its notice before it
ships. MinGW-w64's winpthreads, linked into every Windows DLL, is each DLL's too.

    python tools/check_notices.py <the build's folder> [the repository's folder]
"""

import json
import pathlib
import re
import sys

# Each library a DLL links, by its target's name, and the notices its code needs: what's built
# from it, and what it has inside. A name is matched whole, as a regular expression.
LINKED = {
    "SDL3-static": ["sdl.txt"],
    "freetype": ["freetype.txt"],
    "fbengine_harfbuzz": ["harfbuzz.txt", "harfbuzz-ms-use.txt"],
    "fbengine_sheenbidi": ["sheenbidi.txt"],
    "ktx_read": ["ktx.txt", "basisu.txt", "zstd.txt"],
    "ktx": ["ktx.txt", "basisu.txt", "zstd.txt"],
    "ktx_version": [],
    r"astcenc-[\w.]+-static": ["astc-encoder.txt"],
    "fastgltf": ["fastgltf.txt", "simdjson.txt"],
    "volk": ["volk.txt"],
    "fbengine_miniaudio": ["miniaudio.txt"],
    "Jolt": ["jolt.txt"],
    "meshoptimizer": ["meshoptimizer.txt"],
    "pl_mpeg": ["pl_mpeg.txt"],
    "SPIRV-Tools-static": ["spirv-tools.txt", "spirv-headers.txt"],
}
# Each FetchContent folder, _deps/<name>-src, a DLL's code is compiled from or includes, and its
# notices: header-only libraries are only these.
FOLDERS = {
    "sdl3": ["sdl.txt"],
    "freetype": ["freetype.txt"],
    "harfbuzz": ["harfbuzz.txt", "harfbuzz-ms-use.txt"],
    "sheenbidi": ["sheenbidi.txt"],
    "stb": ["stb.txt"],
    "ktx": ["ktx.txt"],
    "fastgltf": ["fastgltf.txt"],
    "volk": ["volk.txt"],
    "vulkanheaders": ["vulkan-headers.txt"],
    "vulkanmemoryallocator": ["vma.txt"],
    "miniaudio": ["miniaudio.txt"],
    "joltphysics": ["jolt.txt"],
    "meshoptimizer": ["meshoptimizer.txt"],
    "spirv-tools": ["spirv-tools.txt"],
    "spirv-headers": ["spirv-headers.txt"],
}
# A vendored file one of a DLL's own sources (the repository's, or the build's own) #includes, by
# its name, and its notice: what's compiled in that no target or folder says.
INCLUDED = {
    "stb_vorbis.c": ["stb_vorbis.txt"],
    "ma_reverb_node.c": ["verblib.txt"],
    "default_font.inc": ["noto-sans.txt"],
}
# Every DLL's, as cmake/native.cmake links MinGW's winpthreads into each statically.
EVERY_DLL = ["winpthreads.txt"]
# What's no library: the build's own steps.
UTILITY = {"UTILITY"}


def known(table, name):
    """The notices `table` gives `name`, or None if it doesn't know it."""
    for pattern, notices in table.items():
        if re.fullmatch(pattern, name):
            return notices
    return None


def graph(build):
    """Each DLL's targets and what they're made of, from CMake's file API: {package: (the targets it
    links, through every step, the FetchContent folders they're compiled from or include, and their
    own sources)}. None if the build has no reply."""
    reply = build / ".cmake" / "api" / "v1" / "reply"
    indexes = sorted(reply.glob("index-*.json")) if reply.is_dir() else []
    if not indexes:
        return None
    index = json.loads(indexes[-1].read_text(encoding="utf-8"))
    codemodel = next(entry for entry in index["objects"] if entry["kind"] == "codemodel")
    model = json.loads((reply / codemodel["jsonFile"]).read_text(encoding="utf-8"))
    source = pathlib.Path(model["paths"]["source"])
    targets = {target["id"]: target for target in model["configurations"][0]["targets"]}

    def read(target_id):
        return json.loads((reply / targets[target_id]["jsonFile"]).read_text(encoding="utf-8"))

    dlls = {}
    for target in targets.values():
        package = target["name"].removesuffix("_native")
        if package == target["name"]:
            continue
        linked = set()
        folders = set()
        own = set()
        seen = set()
        todo = [target["id"]]
        while todo:
            target_id = todo.pop()
            if target_id in seen:
                continue
            seen.add(target_id)
            made = read(target_id)
            if made["type"] in UTILITY:
                continue
            if target_id != target["id"]:
                linked.add(made["name"])
            todo += [each["id"] for each in made.get("dependencies", [])]
            paths = [each["path"] for each in made.get("sources", [])]
            includes = [each["path"] for group in made.get("compileGroups", []) for each in group.get("includes", [])]
            for path in paths + includes:
                folder = re.search(r"(?:^|/)_deps/([^/]+?)-(?:src|build|subbuild)(?:/|$)", path.replace("\\", "/"))
                if folder:
                    folders.add(folder.group(1))
                elif path in paths:
                    own.add(path if pathlib.Path(path).is_absolute() else str(source / path))
        dlls[package] = (sorted(linked), sorted(folders), sorted(own))
    return dlls


def included(sources):
    """The vendored files INCLUDED knows that `sources` #include, by name."""
    found = set()
    for path in sources:
        try:
            text = pathlib.Path(path).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for name in re.findall(r'^\s*#\s*include\s*["<]([^">]+)[">]', text, re.MULTILINE):
            name = name.rsplit("/", 1)[-1]
            if name in INCLUDED:
                found.add(name)
    return found


def problems(build, root):
    """What's wrong with the notices, and how many DLLs were checked."""
    dlls = graph(build)
    if dlls is None:
        return [f"{build} has no CMake file API reply: configure it with tests/CMakeLists.txt's query"], 0
    wrong = []
    checked = 0
    for folder in sorted(path for path in (root / "packages").iterdir() if path.is_dir()):
        package = folder.name
        if package not in dlls:
            continue
        linked, fetched, own = dlls[package]
        needed = list(EVERY_DLL)
        for library in linked:
            notices = known(LINKED, library)
            if notices is None:
                wrong.append(f"{package}'s DLL links {library}, whose notices this doesn't know: add them, "
                             "and it to LINKED")
                continue
            needed += notices
        for name in fetched:
            notices = known(FOLDERS, name)
            if notices is None:
                wrong.append(f"{package}'s DLL is compiled from _deps/{name}-src, whose notices this doesn't "
                             "know: add them, and it to FOLDERS")
                continue
            needed += notices
        for name in sorted(included(own)):
            needed += INCLUDED[name]
        readme = (folder / "README.md").read_text(encoding="utf-8")
        # A line one space in, after a list, is the last item's, as Markdown renders it.
        for number, line in enumerate(readme.splitlines(), 1):
            if re.match(r" \S", line):
                wrong.append(f"{package}'s README.md:{number} starts one space in, so it renders inside the "
                             "list item above: start it with no space, after a blank line")
        for notice in sorted(set(needed)):
            if not (folder / "notices" / notice).is_file():
                wrong.append(f"{package} has no notices/{notice}")
            elif f"notices/{notice}" not in readme:
                wrong.append(f"{package}'s README doesn't name notices/{notice}")
        kept = sorted((folder / "notices").iterdir()) if (folder / "notices").is_dir() else []
        for path in kept:
            if path.name not in needed:
                wrong.append(f"{package}'s notices/{path.name} is for nothing in its DLL")
        checked += 1
    return wrong, checked


def main():
    if len(sys.argv) not in (2, 3):
        print(__doc__)
        return 2
    build = pathlib.Path(sys.argv[1]).resolve()
    here = pathlib.Path(__file__).resolve().parent.parent
    root = pathlib.Path(sys.argv[2]).resolve() if len(sys.argv) == 3 else here
    wrong, checked = problems(build, root)
    for line in wrong:
        print(line)
    if wrong or checked == 0:
        return 1
    print(f"the notices of {checked} DLLs name every library they link")
    return 0


if __name__ == "__main__":
    sys.exit(main())
