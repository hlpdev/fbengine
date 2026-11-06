"""Tests check_notices.py on repositories and builds of its own, made in a temporary folder: a build's
CMake file API reply as CMake writes it, for one package's DLL, whose notices are complete, then
each way they can fall short.

    python tools/check_notices_test.py
"""

import json
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import check_notices  # noqa: E402


# Each library's FetchContent folder, by its target's name, as the real build has them.
FETCHED = { "Jolt": "joltphysics", "zlibstatic": "zlib" }


def target(reply, name, kind, depends=(), sources=(), includes=()):
    """A target's reply file, and its entry in the codemodel."""
    made = {
        "name": name,
        "id": f"{name}::@1",
        "type": kind,
        "dependencies": [{ "id": f"{each}::@1" } for each in depends],
        "sources": [{ "path": each } for each in sources],
        "compileGroups": [{ "includes": [{ "path": each } for each in includes] }],
    }
    file = f"target-{name}.json"
    (reply / file).write_text(json.dumps(made), encoding="utf-8")
    return { "name": name, "id": made["id"], "jsonFile": file }


def made(root, build, links, includes, own, notices, readme):
    """A repository of one package, fbengine_physics, and its build, whose DLL links `links`, each
    linking the next, includes `includes`, and has the sources `own` (a name and its text each)."""
    package = root / "packages" / "fbengine_physics"
    (package / "notices").mkdir(parents=True)
    for name in notices:
        (package / "notices" / name).write_text("its license\n", encoding="utf-8")
    (package / "README.md").write_text(readme, encoding="utf-8")
    # A package with no DLL has nothing to check.
    (root / "packages" / "fbengine_ui").mkdir()
    for name, text in own:
        path = root / "native" / "fbengine_physics" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    if build is None:
        return
    reply = build / ".cmake" / "api" / "v1" / "reply"
    reply.mkdir(parents=True)
    targets = [target(reply, "fbengine_physics_native", "SHARED_LIBRARY", links[:1],
                      [f"native/fbengine_physics/{name}" for name, _ in own] + ["native/fbengine_physics/area.h"],
                      [f"{build.as_posix()}/_deps/{each}-src/include" for each in includes]),
               target(reply, "fbengine_physics_shaders", "UTILITY")]
    for at, name in enumerate(links):
        targets.append(target(reply, name, "STATIC_LIBRARY", links[at + 1:at + 2],
                              [f"{build.as_posix()}/_deps/{FETCHED.get(name, name)}-src/{name}.cpp"]))
    model = { "paths": { "source": root.as_posix(), "build": build.as_posix() },
              "configurations": [{ "name": "Release", "targets": targets }] }
    (reply / "codemodel-v2-1.json").write_text(json.dumps(model), encoding="utf-8")
    index = { "objects": [{ "kind": "codemodel", "version": { "major": 2 }, "jsonFile": "codemodel-v2-1.json" }] }
    (reply / "index-2026-11-06T00-00-00-0000.json").write_text(json.dumps(index), encoding="utf-8")


def check(name, expected, links=("Jolt",), includes=(), own=(("body.cpp", "#include \"body.h\"\n"),),
          notices=("jolt.txt", "winpthreads.txt"), readme=None, reply=True):
    readme = readme if readme is not None else " ".join(f"(notices/{each})" for each in notices)
    with tempfile.TemporaryDirectory() as folder:
        root = pathlib.Path(folder) / "repository"
        build = pathlib.Path(folder) / "build"
        made(root, build if reply else None, list(links), includes, own, notices, readme)
        wrong, checked = check_notices.problems(build, root)
    said = "\n".join(wrong)
    ok = (checked == 1 or not reply) and (expected in said if expected else not wrong)
    print(f"{name}: {'ok' if ok else 'WRONG'}{'' if ok else ': ' + repr(said)}")
    return ok


def main():
    both = ("jolt.txt", "winpthreads.txt")
    results = [
        check("complete", None),
        check("a notice missing", "has no notices/jolt.txt", notices=("winpthreads.txt",),
              readme="(notices/jolt.txt) (notices/winpthreads.txt)"),
        check("winpthreads missing", "has no notices/winpthreads.txt", notices=("jolt.txt",),
              readme="(notices/jolt.txt) (notices/winpthreads.txt)"),
        check("a library it doesn't know", "links zlibstatic, whose notices this doesn't know",
              links=("Jolt", "zlibstatic")),
        check("a library linked through another, without its notice", "has no notices/meshoptimizer.txt",
              links=("Jolt", "meshoptimizer"), readme="(notices/jolt.txt) (notices/meshoptimizer.txt) "
              "(notices/winpthreads.txt)"),
        check("a header-only library without its notice", "has no notices/vulkan-headers.txt",
              includes=("vulkanheaders",), readme="(notices/jolt.txt) (notices/vulkan-headers.txt) "
              "(notices/winpthreads.txt)"),
        check("a folder it doesn't know", "compiled from _deps/zlib-src, whose notices this doesn't know",
              includes=("zlib",)),
        check("a library compiled in without its notice", "has no notices/stb_vorbis.txt",
              own=(("decode.c", "#define STB_VORBIS_HEADER_ONLY\n#  include \"extras/stb_vorbis.c\"\n"),),
              readme="(notices/jolt.txt) (notices/stb_vorbis.txt) (notices/winpthreads.txt)"),
        check("a library compiled in with its notice", None,
              own=(("decode.c", "#include \"extras/stb_vorbis.c\"\n"),),
              notices=both + ("stb_vorbis.txt",)),
        check("the README not naming one", "README doesn't name notices/jolt.txt",
              readme="(notices/winpthreads.txt)"),
        check("a README line inside the list above", "README.md:2 starts one space in",
              readme="- (notices/jolt.txt) (notices/winpthreads.txt)\n Portions of this software\n"),
        check("a notice for nothing", "notices/pl_mpeg.txt is for nothing", notices=both + ("pl_mpeg.txt",)),
        check("a build with no reply", "has no CMake file API reply", reply=False),
    ]
    if not all(results):
        return 1
    print(f"all {len(results)} cases hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
