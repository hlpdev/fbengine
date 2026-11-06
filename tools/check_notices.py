"""Checks each package's notices name every third-party library its DLL links: each DLL's
target_link_libraries in native/<package>/CMakeLists.txt, with the lists it set()s there, is read
for what it links, and each of those has the notices its code asks for in packages/<package>/notices/,
which the package's README names. A library this doesn't know stops it, so a new one gets its notice
before it ships. MinGW-w64's winpthreads, linked into every Windows DLL, and what a DLL embeds that
isn't linked, Font.BuiltIn's font, are each DLL's too.

    python tools/check_notices.py [the repository's folder]
"""

import pathlib
import re
import sys

# Each library a DLL links, by its name in target_link_libraries, and the notices its code needs:
# what's built from it, and what it has inside.
NOTICES = {
    "SDL3::SDL3-static": ["sdl.txt"],
    "freetype": ["freetype.txt"],
    "fbengine_harfbuzz": ["harfbuzz.txt", "harfbuzz-ms-use.txt"],
    "fbengine_sheenbidi": ["sheenbidi.txt"],
    "fbengine_stb": ["stb.txt"],
    "ktx_read": ["ktx.txt", "basisu.txt", "zstd.txt"],
    "ktx": ["ktx.txt", "basisu.txt", "astc-encoder.txt", "zstd.txt"],
    "fastgltf::fastgltf": ["fastgltf.txt", "simdjson.txt"],
    "volk": ["volk.txt"],
    "Vulkan::Headers": ["vulkan-headers.txt"],
    "fbengine_vma": ["vma.txt"],
    "fbengine_miniaudio": ["miniaudio.txt"],
    "Jolt": ["jolt.txt"],
    "meshoptimizer": ["meshoptimizer.txt"],
    "pl_mpeg": ["pl_mpeg.txt"],
}
# What's the system's, which needs no notice.
SYSTEM = {"Threads::Threads", "${CMAKE_DL_LIBS}", "m"}
# Every DLL's, as cmake/native.cmake links MinGW's winpthreads into each statically.
EVERY_DLL = ["winpthreads.txt"]
# What a DLL embeds in its own sources, by the source that does, and its notice.
EMBEDDED = {"fbengine": [("native/fbengine/text/default_font.inc", "noto-sans.txt")]}


def calls(text, name):
    """The words of each call of `name` in a CMake file, its comments left out."""
    text = re.sub(r"#[^\n]*", "", text)
    return [body.split() for body in re.findall(rf"\b{name}\s*\(([^)]*)\)", text)]


def linked(root, package):
    """What the package's DLL links, or None if it has no DLL."""
    cmake = root / "native" / package / "CMakeLists.txt"
    if not cmake.is_file():
        return None
    text = cmake.read_text(encoding="utf-8")
    dll = f"{package}_native"
    if not any(words and words[0] == dll for words in calls(text, "fbengine_add_native")):
        return None
    lists = {words[0]: words[1:] for words in calls(text, "set") if words}
    found = []
    for words in calls(text, "target_link_libraries"):
        if not words or words[0] != dll:
            continue
        for word in words[1:]:
            if word in ("PRIVATE", "PUBLIC", "INTERFACE"):
                continue
            listed = re.fullmatch(r"\$\{(\w+)\}", word)
            if listed and listed.group(1) in lists:
                found += lists[listed.group(1)]
            else:
                found.append(word)
    return found


def problems(root):
    """What's wrong with the notices, and how many DLLs were checked."""
    wrong = []
    checked = 0
    for folder in sorted(path for path in (root / "packages").iterdir() if path.is_dir()):
        package = folder.name
        libraries = linked(root, package)
        if libraries is None:
            continue
        needed = list(EVERY_DLL)
        for library in libraries:
            if library in SYSTEM:
                continue
            if library not in NOTICES:
                wrong.append(f"{package}'s DLL links {library}, whose notices this doesn't know: add them, "
                             "and it to NOTICES")
                continue
            needed += NOTICES[library]
        for source, notice in EMBEDDED.get(package, []):
            if not (root / source).is_file():
                wrong.append(f"{package} embeds {source}, which isn't there: take it out of EMBEDDED")
            needed.append(notice)
        readme = (folder / "README.md").read_text(encoding="utf-8")
        for notice in sorted(set(needed)):
            if not (folder / "notices" / notice).is_file():
                wrong.append(f"{package} has no notices/{notice}")
            elif f"notices/{notice}" not in readme:
                wrong.append(f"{package}'s README doesn't name notices/{notice}")
        kept = sorted((folder / "notices").iterdir()) if (folder / "notices").is_dir() else []
        for path in kept:
            if path.name not in needed:
                wrong.append(f"{package}'s notices/{path.name} is for nothing its DLL links")
        checked += 1
    return wrong, checked


def main():
    if len(sys.argv) > 2:
        print(__doc__)
        return 2
    here = pathlib.Path(__file__).resolve().parent.parent
    root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else here
    wrong, checked = problems(root)
    for line in wrong:
        print(line)
    if wrong or checked == 0:
        return 1
    print(f"the notices of {checked} DLLs name every library they link")
    return 0


if __name__ == "__main__":
    sys.exit(main())
