"""Whether the engine's shaders come out as Metal that could compile, on a machine with no Metal:
each built shader through SPIRV-Cross, as MoltenVK runs it, looking for a member read through a
pointer with a '.', which Metal refuses (`a->b[i].pointer.member`). It's a look at the text, not a
compile, which only CI's Macs do. With no spirv-cross here, from a Vulkan SDK or a package, it says
so and passes.

    python tools/check_msl.py <build folder>
"""

import os
import pathlib
import re
import shutil
import subprocess
import sys


def spirv_cross():
    found = shutil.which("spirv-cross")
    if found:
        return found
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        for name in ("Bin/spirv-cross.exe", "bin/spirv-cross"):
            if (pathlib.Path(sdk) / name).exists():
                return str(pathlib.Path(sdk) / name)
    return shutil.which("spirv-cross", path=";".join(str(p / "Bin") for p in pathlib.Path("C:/VulkanSDK").glob("*")))


def bad_lines(metal):
    """The lines that read a member through a pointer member with a '.'."""
    pointers = set(re.findall(r"^\s*(?:const )?device [\w:<>, ]+\*\s*(\w+);", metal, re.MULTILINE))
    if not pointers:
        return []
    # Through a pointer to it, not a struct's own member of that name, as Joints has `joints`.
    through = re.compile(r"->\w+\[[^\]]*\]\.(" + "|".join(sorted(pointers)) + r")\.\w")
    return [line.strip()[:160] for line in metal.splitlines() if through.search(line)]


def main():
    tool = spirv_cross()
    if tool is None:
        print("no spirv-cross here, so the shaders' Metal isn't looked at")
        return 0
    failed = False
    build = pathlib.Path(sys.argv[1])
    # A game's shader too, whose draws read through the same addresses.
    shaders = sorted((build / "native").rglob("*.spv")) + sorted((build / "tests").rglob("game_shader.spv"))
    for shader in shaders:
        listed = subprocess.run([tool, str(shader), "--reflect"], capture_output=True, text=True, check=True)
        entries = re.findall(r'"name" : "(\w+)",\s*"mode" : "(\w+)"', listed.stdout)
        for name, mode in entries:
            # The bindless set, which only comes out at these.
            done = subprocess.run([tool, str(shader), "--msl", "--msl-version", "30000", "--entry", name,
                                   "--stage", mode, "--msl-argument-buffers", "--msl-argument-buffer-tier", "1",
                                   "--msl-device-argument-buffer", "0"],
                                  capture_output=True, text=True, check=False)
            if done.returncode != 0:
                print(f"{shader.name} {name}: spirv-cross failed: {done.stderr[:300]}")
                failed = True
                continue
            for line in bad_lines(done.stdout):
                print(f"{shader.name} {name}: {line}")
                failed = True
    print(f"looked at {len(shaders)} shaders' Metal")
    return 1 if failed else 0


sys.exit(main())
