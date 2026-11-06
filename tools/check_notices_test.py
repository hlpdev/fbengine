"""Tests check_notices.py on repositories of its own, made in a temporary folder: one whose notices
are complete, then each way they can fall short.

    python tools/check_notices_test.py
"""

import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import check_notices  # noqa: E402


def made(root, links, notices, readme):
    """A repository of one package, fbengine_physics, whose DLL links `links`."""
    native = root / "native" / "fbengine_physics"
    native.mkdir(parents=True)
    (native / "CMakeLists.txt").write_text(
        "# What it links: Jolt\n"
        "set(physics_links " + " ".join(links) + ")\n"
        "fbengine_add_native(fbengine_physics_native fbengine_physics fbe_ ${sources})\n"
        "target_link_libraries(fbengine_physics_native PRIVATE ${physics_links} Threads::Threads)\n",
        encoding="utf-8")
    package = root / "packages" / "fbengine_physics"
    (package / "notices").mkdir(parents=True)
    for name in notices:
        (package / "notices" / name).write_text("its license\n", encoding="utf-8")
    (package / "README.md").write_text(readme, encoding="utf-8")
    # A package with no DLL has nothing to check.
    (root / "packages" / "fbengine_ui").mkdir()


def check(name, links, notices, readme, expected):
    with tempfile.TemporaryDirectory() as folder:
        root = pathlib.Path(folder)
        made(root, links, notices, readme)
        wrong, checked = check_notices.problems(root)
    said = "\n".join(wrong)
    ok = checked == 1 and (expected in said if expected else not wrong)
    print(f"{name}: {'ok' if ok else 'WRONG'}{'' if ok else ': ' + repr(said)}")
    return ok


def main():
    named = "Jolt (notices/jolt.txt), winpthreads (notices/winpthreads.txt)\n"
    both = ["jolt.txt", "winpthreads.txt"]
    results = [
        check("complete", ["Jolt"], both, named, None),
        check("a notice missing", ["Jolt"], ["winpthreads.txt"], named, "has no notices/jolt.txt"),
        check("winpthreads missing", ["Jolt"], ["jolt.txt"], named, "has no notices/winpthreads.txt"),
        check("a library it doesn't know", ["Jolt", "zlibstatic"], both, named,
              "links zlibstatic, whose notices this doesn't know"),
        check("the README not naming one", ["Jolt"], both, "notices/winpthreads.txt\n",
              "README doesn't name notices/jolt.txt"),
        check("a notice for nothing", ["Jolt"], both + ["pl_mpeg.txt"], named,
              "notices/pl_mpeg.txt is for nothing"),
    ]
    if not all(results):
        return 1
    print(f"all {len(results)} cases hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
