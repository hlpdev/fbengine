"""Checks each library package publishes, uploading nothing: `fb publish --dry-run` of each, in the
order publish.yml publishes them, with futureblitz.dev's registry a folder of its own. The packages
are copied into a temporary folder, with the DLLs they have, and each copy's project.toml names
that folder as futureblitz's, so a package's registry dependencies are found there; after each dry
run, the package is published into the folder, for the next one's to find. publish.yml's order is
the one checked, so a package published before what it needs fails here first.

    python tools/check_publish.py <fb> [the repository's folder]
"""

import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile


def order(root):
    """The packages publish.yml publishes, in its order."""
    workflow = (root / ".github" / "workflows" / "publish.yml").read_text(encoding="utf-8")
    found = re.search(r"for package in ((?:[\w ]|\\\n)+); do\s*\n\s*fb publish \"packages/\$package\"", workflow)
    if found is None:
        raise SystemExit("publish.yml has no `for package in ...; do fb publish \"packages/$package\"` loop")
    return found.group(1).replace("\\\n", " ").split()


def run(fb, folder, *words):
    done = subprocess.run([fb, "--color", "never", "publish", *words], cwd=folder, capture_output=True,
                          text=True, encoding="utf-8", errors="replace")
    return done.returncode, done.stdout + done.stderr


def main():
    if len(sys.argv) not in (2, 3):
        print(__doc__)
        return 2
    fb = sys.argv[1]
    root = pathlib.Path(sys.argv[2] if len(sys.argv) == 3 else pathlib.Path(__file__).parent.parent).resolve()
    packages = order(root)
    if len(set(packages)) != len(packages):
        print(f"publish.yml publishes a package twice: {packages}")
        return 1
    with tempfile.TemporaryDirectory() as folder:
        temp = pathlib.Path(folder)
        registry = temp / "registry"
        registry.mkdir()
        (registry / "config.json").write_text(json.dumps({ "v": 1, "dl": "dl/{name}/{version}/{file}" }),
                                              encoding="utf-8")
        index = registry.as_posix()
        copies = temp / "packages"
        for package in packages:
            copy = copies / package
            shutil.copytree(root / "packages" / package, copy, ignore=shutil.ignore_patterns("build"))
            toml = copy / "project.toml"
            text = toml.read_text(encoding="utf-8")
            if "[registries]" in text:
                print(f"{package}'s project.toml has [registries] already, which this can't add to")
                return 1
            mirror = f'\n\n[registries]\nfutureblitz = {{ index = "{index}" }}\n'
            toml.write_text(text.rstrip("\n") + mirror, encoding="utf-8")
        for package in packages:
            copy = copies / package
            code, said = run(fb, copies, "--dry-run", str(copy), "--registry", index)
            if code != 0 or "Finished the dry run" not in said:
                print(f"{package}'s dry run failed:\n{said[-3000:]}")
                return 1
            print(f"{package}: its dry run passed")
            code, said = run(fb, copies, str(copy), "--registry", index)
            if code != 0:
                print(f"{package} didn't go into the folder's registry:\n{said[-3000:]}")
                return 1
    print(f"all {len(packages)} libraries' dry runs passed, in publish.yml's order")
    return 0


if __name__ == "__main__":
    sys.exit(main())
