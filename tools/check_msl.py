"""Whether the engine's shaders come out as Metal that could compile, on a machine with no Metal:
each built shader through SPIRV-Cross, as MoltenVK runs it, looking for a member read through a
pointer with a '.', which Metal refuses (`a->b[i].pointer.member`, `a.b[i].pointer.member`). It's a
look at the text, not a compile, which only CI's Macs do. With no spirv-cross here, from a Vulkan SDK or a package, it says
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


def members_of(metal):
    """Each struct's members, each with the type it points to, None for what isn't a pointer."""
    structs = {}
    for name, body in re.findall(r"^struct (\w+)\n\{\n(.*?)^\};", metal, re.MULTILINE | re.DOTALL):
        members = {}
        for line in body.splitlines():
            pointer = re.match(r"\s*(?:const )?device (\w+)\*\s*(?:const\s+)?(\w+);", line)
            if pointer:
                members[pointer.group(2)] = pointer.group(1)
                continue
            plain = re.match(r".*?(\w+)(?:\s*\[[^\]]*\])?;\s*$", line)
            if plain:
                members[plain.group(1)] = None
        structs[name] = members
    return structs


def subscripted(line, end):
    """The name subscripted by the brackets ending just before `end`, if they do."""
    if end == 0 or line[end - 1] != "]":
        return None
    depth = 0
    for at in range(end - 1, -1, -1):
        depth += {"]": 1, "[": -1}.get(line[at], 0)
        if depth == 0:
            found = re.search(r"(\w+)$", line[:at])
            return found.group(1) if found else None
    return None


def bad_lines(metal):
    """The lines that read a member through a pointer member with a '.', however they got to it,
    as `a->b[i].pointer.member` and `a.b[i].pointer.member` both are."""
    structs = members_of(metal)
    points_to = {}
    plain = set()
    for members in structs.values():
        for name, target in members.items():
            if target is None:
                plain.add(name)
            else:
                points_to.setdefault(name, set()).add(target)
    bad = []
    for line in metal.splitlines():
        for found in re.finditer(r"(?:\.|->)(\w+)\.\w", line):
            name = found.group(1)
            if name not in points_to:
                continue
            # A struct's own member of that name, as Joints has `joints`, goes by what it's in.
            if name in plain:
                through = subscripted(line, found.start())
                targets = points_to.get(through, set())
                if not any(structs.get(target, {}).get(name) for target in targets):
                    continue
            bad.append(line.strip()[:160])
            break
    return bad


# Which is which, in what SPIRV-Cross writes, whatever the shaders are now: a read of each kind.
EXAMPLE = """struct Particle_natural
{
    float life;
};
struct Emitter_natural
{
    const device Particle_natural* records;
    float now;
};
struct Joints_natural
{
    spvUnsafeArray<uint, 2> joints;
};
struct Draw_natural
{
    const device Emitter_natural* emitter;
    const device Joints_natural* joints;
    const device Particle_natural* records;
};
struct Camera_natural
{
    const device Draw_natural* draws;
};
"""
EXAMPLE_BAD = [
    "float a = (*reinterpret_cast<const device Draw_natural*>(x)).emitter[0 + i].records.life;",
    "float a = push.camera->draws[i].records.life;",
    "uint a = push.camera->draws[i].joints.joints.data[0u];",
]
EXAMPLE_FINE = [
    "float a = (*reinterpret_cast<const device Draw_natural*>(x)).emitter[0].now;",
    "float a = spvDescriptorSet0.emitter->records[i].life;",
    "uint a = joints[index].joints.data[0u];",
    "uint a = _123.joints.data[0u];",
]


def check_itself():
    """Whether it finds what it's to find, so it can't pass by finding nothing."""
    right = True
    for line in EXAMPLE_BAD:
        right = right and bool(bad_lines(EXAMPLE + line))
    for line in EXAMPLE_FINE:
        right = right and not bad_lines(EXAMPLE + line)
    return right


def main():
    if not check_itself():
        print("check_msl.py doesn't tell Metal's bad reads from its good ones")
        return 1
    tool = spirv_cross()
    if tool is None:
        print("no spirv-cross here, so the shaders' Metal isn't looked at")
        return 0
    failed = False
    build = pathlib.Path(sys.argv[1])
    # A game's shader too, whose draws read through the same addresses.
    shaders = sorted((build / "native").rglob("*.spv")) + sorted((build / "tests").rglob("game_shader.spv"))
    # And a game's full-screen pass, whose passFragment reads through its own address.
    shaders += sorted((build / "tests").rglob("game_pass.spv"))
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
