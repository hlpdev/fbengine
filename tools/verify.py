"""Checks fbengine before a step lands: every lane's check, and every landing's.

    python tools/verify.py [--build <folder>] [--base <ref>] [--no-build] [--no-tidy]

It builds (configuring the folder first if it's new), runs ctest (the C++ tests, the packages'
`fb test`, and the examples, every test offscreen), checks each package's DLL exports only its C
API, checks the bindings are what tools/bindgen.sh makes, runs clang-format on every C and C++
file, clang-tidy on the C++ files that changed since --base (all of an area's when its header
changed), and `fb fmt --check` on the packages and examples. It ends with "everything passed", or
how many checks failed; a landing's push waits on that line.

clang-format and clang-tidy come from $CLANG_FORMAT and $CLANG_TIDY, or PATH, or where the user's
machine has them.
"""

import argparse
import os
import pathlib
import platform
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FALLBACKS = {
    "CLANG_FORMAT": "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin/clang-format.exe",
    "CLANG_TIDY": "C:/Users/Hunter/AppData/Local/Programs/CLion/bin/clang/win/x64/bin/clang-tidy.exe",
}
# The clang-format CI pins (ci.yml), which every check formats with.
CLANG_FORMAT_VERSION = "19.1.5"
failures = []


def step(name):
    print(f"\n== {name}", flush=True)


def fail(message):
    print(f"FAIL: {message}", flush=True)
    failures.append(message)


def run(command, **options):
    """Runs `command`, its output shown as it goes; its exit code."""
    return subprocess.run(command, cwd=options.pop("cwd", ROOT), **options).returncode


def output(command):
    return subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False).stdout


def tool(variable, name):
    found = os.environ.get(variable) or shutil.which(name)
    if found is None and pathlib.Path(FALLBACKS[variable]).exists():
        found = FALLBACKS[variable]
    return found


def sources():
    """Every C and C++ file of ours, tracked or new, not a dependency's."""
    patterns = ["*.h", "*.hpp", "*.c", "*.cpp"]
    files = output(["git", "ls-files", *patterns]).split()
    files += output(["git", "ls-files", "--others", "--exclude-standard", *patterns]).split()
    return sorted({file for file in files if not file.startswith("build")})


def pinned_clang_format():
    """clang-format at the version CI pins, which formats as CI does; another version formats some
    lines differently, so it's never used."""
    candidates = [os.environ.get("CLANG_FORMAT"), FALLBACKS["CLANG_FORMAT"], shutil.which("clang-format")]
    for candidate in candidates:
        if candidate and pathlib.Path(candidate).exists() or (candidate and shutil.which(candidate)):
            version = output([candidate, "--version"])
            if f"version {CLANG_FORMAT_VERSION}" in version:
                return candidate
    return None


def changed_cpp(base):
    """The .cpp files to tidy: those that changed since `base`, and every .cpp of an area whose
    header changed."""
    changed = set(output(["git", "diff", "--name-only", base, "--", "*.h", "*.hpp", "*.cpp"]).split())
    changed |= set(output(["git", "ls-files", "--others", "--exclude-standard", "*.cpp"]).split())
    files = {file for file in changed if file.endswith(".cpp") and (ROOT / file).exists()}
    for header in changed:
        if header.endswith((".h", ".hpp")):
            area = pathlib.PurePosixPath(header).parent
            files |= {file for file in output(["git", "ls-files", f"{area}/*.cpp"]).split()}
    return sorted(files)


def check_build(build):
    step("build")
    if not (build / "CMakeCache.txt").exists():
        if run(["cmake", "-S", ".", "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug"]) != 0:
            fail("cmake couldn't configure")
            return False
    if run(["cmake", "--build", str(build)]) != 0:
        fail("the build failed")
        return False
    return True


def check_tests(build):
    step("ctest")
    environment = dict(os.environ, SDL_VIDEO_DRIVER="offscreen")
    if run(["ctest", "--test-dir", str(build), "--output-on-failure"], env=environment) != 0:
        fail("ctest failed")


def this_target():
    """This machine's target, as FutureBlitz names it, whose DLLs the build made."""
    system = {"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux")
    machine = platform.machine().lower()
    return f"{system}-{'arm64' if machine in ('arm64', 'aarch64') else 'x64'}"


def check_exports():
    step("exports")
    for native in sorted((ROOT / "native").iterdir()):
        # Other targets' DLLs, from CI's artifacts, are only checked on their own machines.
        folder = ROOT / "packages" / native.name / "native" / this_target()
        dlls = [file for file in folder.rglob("*") if file.suffix in (".dll", ".so", ".dylib")]
        if not dlls:
            fail(f"{native.name} has no built DLL in {folder.relative_to(ROOT)}")
        for dll in dlls:
            if run(["sh", "tools/check-exports.sh", dll.relative_to(ROOT).as_posix(), "fbe_"]) != 0:
                fail(f"{dll.name} exports more than its C API")


def check_bindings():
    step("bindings")
    if run(["sh", "tools/bindgen.sh", "--check"]) != 0:
        fail("the bindings aren't what their headers make; run tools/bindgen.sh")


def check_format():
    step(f"clang-format {CLANG_FORMAT_VERSION}")
    clang_format = pinned_clang_format()
    if clang_format is None:
        fail(f"no clang-format {CLANG_FORMAT_VERSION}, the version CI pins; set CLANG_FORMAT to one")
        return
    files = sources()
    if files and run([clang_format, "--dry-run", "-Werror", *files]) != 0:
        fail("clang-format would change files")


def check_tidy(build, base):
    step(f"clang-tidy on what changed since {base}")
    clang_tidy = tool("CLANG_TIDY", "clang-tidy")
    if clang_tidy is None:
        fail("clang-tidy isn't on PATH; set CLANG_TIDY")
        return
    files = changed_cpp(base)
    if not files:
        print("no C++ changed")
        return
    for file in files:
        result = subprocess.run([clang_tidy, "-p", str(build), "--quiet", file], cwd=ROOT,
                                capture_output=True, text=True, check=False)
        findings = [line for line in result.stdout.splitlines() if "warning:" in line or "error:" in line]
        if findings:
            print("\n".join(findings))
            fail(f"clang-tidy found {len(findings)} in {file}")
        else:
            print(f"ok   {file}")


def check_fb_format():
    step("fb fmt")
    if run(["fb", "fmt", "--check", "packages", "examples"]) != 0:
        fail("fb fmt would change files")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", default="build", help="the build folder (default: build)")
    parser.add_argument("--base", default="origin/master", help="what clang-tidy diffs against")
    parser.add_argument("--no-build", action="store_true", help="use the build as it is")
    parser.add_argument("--no-tidy", action="store_true", help="skip clang-tidy")
    options = parser.parse_args()
    build = (ROOT / options.build).resolve()

    built = options.no_build or check_build(build)
    if built:
        check_tests(build)
        check_exports()
    check_bindings()
    check_format()
    if not options.no_tidy and built:
        check_tidy(build, options.base)
    check_fb_format()

    print()
    if failures:
        print(f"{len(failures)} failure(s):")
        for message in failures:
            print(f"  {message}")
        sys.exit(1)
    print("everything passed")


if __name__ == "__main__":
    main()
