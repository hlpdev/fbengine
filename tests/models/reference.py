"""Works out, from the glTF specification, with none of the engine's code, where each of the sample
models' frames should be: its vertices' box and mean in the world, over its triangles' corners, and
exactly which pixels they cover. All it takes of the engine is how its camera sees (60 degrees up
its view, down its -Z) and what it names an unnamed node. It writes worked_out.fb and masks.bin,
which main.fb compares what the engine does with. Run it again when FRAMES changes:

    python tests/models/reference.py

With --check, as ctest runs it, it writes nothing, and says if what's there isn't what it works out.
"""

import base64
import json
import math
import pathlib
import struct
import sys

HERE = pathlib.Path(__file__).resolve().parent
SIZE = 256
# The sample's default camera: 60 degrees up its view.
FIELD = math.radians(60.0)

# Its folder, its file, the mesh's node, then for each frame, the time to pose its first clip at, or
# None for none, the weights to set, or None for its own, and the camera's place and where it looks.
FRAMES = [
    # Between keys off their middles, past its last key, and for RiggedSimple before its first and
    # off its keys, which 0.5, 1, and 1.5 are on.
    ("SimpleSkin", "SimpleSkin.gltf", 0, [(t, None) for t in (0.0, 0.1, 0.25, 1.0, 4.0, 6.0)],
     (0, 1, 6), (0, 1, 0)),
    ("SimpleMorph", "SimpleMorph.gltf", 0,
     [(None, None), (0.2, None), (0.5, None), (1.3, None), (1.5, None), (2.5, None), (2.8, None)],
     (0.5, 1, 4), (0.5, 1, 0)),
    ("RiggedSimple", "RiggedSimple.glb", 2, [(t, None) for t in (0.0, 0.5, 1.0, 1.02, 1.5)],
     (0, 0, 22), (0, 0, 0)),
    ("MorphPrimitivesTest", "MorphPrimitivesTest.glb", 1,
     [(None, None), (None, [1.0]), (None, [0.0])], (0, 1.2, 1.2), (0, 0, 0)),
]

COMPONENTS = {5126: ("f", 4), 5123: ("H", 2), 5125: ("I", 4), 5121: ("B", 1)}
WIDTHS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def load(path):
    raw = path.read_bytes()
    if path.suffix == ".glb":
        length = struct.unpack("<I", raw[12:16])[0]
        return json.loads(raw[20:20 + length]), [raw[20 + length + 8:]]
    doc = json.loads(raw)
    return doc, [base64.b64decode(buffer["uri"].split(",", 1)[1]) for buffer in doc["buffers"]]


def read(doc, data, index):
    accessor = doc["accessors"][index]
    view = doc["bufferViews"][accessor["bufferView"]]
    code, size = COMPONENTS[accessor["componentType"]]
    width = WIDTHS[accessor["type"]]
    stride = view.get("byteStride", size * width)
    start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    found = []
    for i in range(accessor["count"]):
        found.append(struct.unpack_from(f"<{width}{code}", data[view["buffer"]], start + i * stride))
    return found


def identity():
    return [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]


def multiply(a, b):
    """Column-major, as glTF's are: a then applied after b."""
    return [sum(a[r + 4 * k] * b[k + 4 * c] for k in range(4)) for c in range(4) for r in range(4)]


def compose(t, q, s):
    x, y, z, w = q
    return [(1 - 2 * (y * y + z * z)) * s[0], 2 * (x * y + z * w) * s[0], 2 * (x * z - y * w) * s[0], 0,
            2 * (x * y - z * w) * s[1], (1 - 2 * (x * x + z * z)) * s[1], 2 * (y * z + x * w) * s[1], 0,
            2 * (x * z + y * w) * s[2], 2 * (y * z - x * w) * s[2], (1 - 2 * (x * x + y * y)) * s[2], 0,
            t[0], t[1], t[2], 1.0]


def point(m, p):
    return tuple(m[r] * p[0] + m[r + 4] * p[1] + m[r + 8] * p[2] + m[r + 12] for r in range(3))


def slerp(a, b, t):
    dot = sum(x * y for x, y in zip(a, b))
    if dot < 0:
        b = tuple(-x for x in b)
        dot = -dot
    if dot > 0.9995:
        mixed = [x + (y - x) * t for x, y in zip(a, b)]
    else:
        angle = math.acos(dot)
        mixed = [(x * math.sin((1 - t) * angle) + y * math.sin(t * angle)) / math.sin(angle)
                 for x, y in zip(a, b)]
    size = math.sqrt(sum(x * x for x in mixed))
    return tuple(x / size for x in mixed)


def sample(times, values, width, time, rotation):
    """glTF's LINEAR, held before its first key and after its last."""
    at = lambda i: values[i * width:(i + 1) * width]
    if time <= times[0]:
        return at(0)
    if time >= times[-1]:
        return at(len(times) - 1)
    for i in range(len(times) - 1):
        if times[i] <= time <= times[i + 1]:
            t = (time - times[i]) / (times[i + 1] - times[i])
            if rotation:
                return slerp(at(i), at(i + 1), t)
            return tuple(x + (y - x) * t for x, y in zip(at(i), at(i + 1)))
    raise AssertionError(time)


def linear_only(sampler):
    """What's worked out here is glTF's LINEAR, so a file of anything else is refused."""
    interpolation = sampler.get("interpolation", "LINEAR")
    if interpolation != "LINEAR":
        raise ValueError(f"{interpolation} isn't worked out here")


def pose(doc, data, time):
    """Each node's local parts, and its weights, as its first animation has them at `time`."""
    parts = []
    for node in doc["nodes"]:
        parts.append({
            "t": node.get("translation", (0, 0, 0)),
            "r": node.get("rotation", (0, 0, 0, 1)),
            "s": node.get("scale", (1, 1, 1)),
            "m": node.get("matrix"),
            "w": node.get("weights"),
        })
    if time is None or "animations" not in doc:
        return parts
    for channel in doc["animations"][0]["channels"]:
        sampler = doc["animations"][0]["samplers"][channel["sampler"]]
        linear_only(sampler)
        times = [t[0] for t in read(doc, data, sampler["input"])]
        out = [x for value in read(doc, data, sampler["output"]) for x in value]
        node = channel["target"]["node"]
        path = channel["target"]["path"]
        if path == "weights":
            width = len(out) // len(times)
            parts[node]["w"] = sample(times, out, width, time, False)
        else:
            key = {"translation": "t", "rotation": "r", "scale": "s"}[path]
            found = sample(times, out, 4 if key == "r" else 3, time, key == "r")
            parts[node][key] = found
            parts[node]["m"] = None
    return parts


def worlds(doc, parts):
    made = [None] * len(doc["nodes"])

    def walk(index, parent):
        node = parts[index]
        local = node["m"] if node["m"] is not None else compose(node["t"], node["r"], node["s"])
        made[index] = multiply(parent, local)
        for child in doc["nodes"][index].get("children", []):
            walk(child, made[index])

    for root in doc["scenes"][doc.get("scene", 0)]["nodes"]:
        walk(root, identity())
    return made


def corners(doc, data, node_index, time, weights):
    """Every triangle corner's place in the world."""
    parts = pose(doc, data, time)
    world = worlds(doc, parts)
    node = doc["nodes"][node_index]
    mesh = doc["meshes"][node["mesh"]]
    used = weights if weights is not None else (parts[node_index]["w"] or mesh.get("weights", []))
    placed = []
    palette = None
    if "skin" in node:
        skin = doc["skins"][node["skin"]]
        binds = read(doc, data, skin["inverseBindMatrices"])
        # A skinned mesh's own node doesn't move it: only its joints do.
        palette = [multiply(world[j], list(binds[i])) for i, j in enumerate(skin["joints"])]
    for primitive in mesh["primitives"]:
        positions = read(doc, data, primitive["attributes"]["POSITION"])
        moved = [list(p) for p in positions]
        for t, target in enumerate(primitive.get("targets", [])):
            if t < len(used) and "POSITION" in target:
                for i, by in enumerate(read(doc, data, target["POSITION"])):
                    for k in range(3):
                        moved[i][k] += by[k] * used[t]
        final = []
        if palette is not None:
            joints = read(doc, data, primitive["attributes"]["JOINTS_0"])
            shares = read(doc, data, primitive["attributes"]["WEIGHTS_0"])
            for i, p in enumerate(moved):
                at = [0.0, 0.0, 0.0]
                for k in range(4):
                    if shares[i][k] != 0.0:
                        q = point(palette[joints[i][k]], p)
                        at = [a + b * shares[i][k] for a, b in zip(at, q)]
                final.append(tuple(at))
        else:
            final = [point(world[node_index], p) for p in moved]
        indices = [i[0] for i in read(doc, data, primitive["indices"])]
        placed.extend(final[i] for i in indices)
    return placed


def covered(places, camera, look):
    """Which pixels the triangles of the places cover, front or back, a row after another, each
    pixel by its middle, as a rasterizer takes it."""
    back = [c - l for c, l in zip(camera, look)]
    size = math.sqrt(sum(x * x for x in back))
    back = [x / size for x in back]
    up = (0.0, 1.0, 0.0)
    right = [up[1] * back[2] - up[2] * back[1], up[2] * back[0] - up[0] * back[2],
             up[0] * back[1] - up[1] * back[0]]
    size = math.sqrt(sum(x * x for x in right))
    right = [x / size for x in right]
    above = [back[1] * right[2] - back[2] * right[1], back[2] * right[0] - back[0] * right[2],
             back[0] * right[1] - back[1] * right[0]]
    per = (SIZE / 2) / math.tan(FIELD / 2)
    seen = []
    for p in places:
        d = [a - c for a, c in zip(p, camera)]
        depth = -sum(a * b for a, b in zip(d, back))
        seen.append((SIZE / 2 + sum(a * b for a, b in zip(d, right)) * per / depth,
                     SIZE / 2 - sum(a * b for a, b in zip(d, above)) * per / depth))
    mask = bytearray(SIZE * SIZE)
    for t in range(0, len(seen), 3):
        (ax, ay), (bx, by), (cx, cy) = seen[t:t + 3]
        twice = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay)
        if twice == 0.0:
            continue
        for y in range(max(0, math.floor(min(ay, by, cy))), min(SIZE, math.ceil(max(ay, by, cy)) + 1)):
            py = y + 0.5
            for x in range(max(0, math.floor(min(ax, bx, cx))), min(SIZE, math.ceil(max(ax, bx, cx)) + 1)):
                px = x + 0.5
                one = ((bx - px) * (cy - py) - (cx - px) * (by - py)) / twice
                two = ((cx - px) * (ay - py) - (ax - px) * (cy - py)) / twice
                if one >= 0 and two >= 0 and one + two <= 1:
                    mask[y * SIZE + x] = 1
    return mask


def runs_of(mask):
    """A mask's runs, each how many pixels, starting with those it doesn't cover."""
    runs = []
    value = 0
    count = 0
    for pixel in mask:
        if pixel != value:
            runs.append(count)
            value = pixel
            count = 0
        count += 1
    runs.append(count)
    return runs


def number(value):
    return f"{round(value, 4) + 0.0:g}"


def main():
    # A sampler it can't work out is refused, not worked out as if it were LINEAR.
    try:
        linear_only({"interpolation": "STEP"})
    except ValueError:
        pass
    else:
        sys.exit("a STEP sampler should be refused")
    masks = bytearray()
    lines = ["// Written by reference.py, from the glTF specification: where each frame should be.",
             "", "Import std.math", "", "// A frame of a sample model, and where it should be.",
             "Package Struct Worked", "    Package Field Folder: String", "    Package Field Node: String",
             "    Package Field Index: Int", "    Package Field Time: Float", "    Package Field Weight: Float",
             "    Package Field Camera: Vec3", "    Package Field Look: Vec3", "    Package Field Low: Vec3",
             "    Package Field High: Vec3", "    Package Field Mean: Vec3",
             "    // Where its mask's runs start in masks.bin, and how many.", "    Package Field Runs: Int",
             "    Package Field RunCount: Int",
             "End Struct", "", "// Time and Weight are below 0 for none.", "Package Function WorkedOut(): Worked[]",
             "    Local frames: Worked[] = []"]
    for folder, file, node_index, frames, camera, look in FRAMES:
        doc, data = load(HERE / folder / file)
        names = doc["nodes"][node_index].get("name", f"node_{node_index}")
        for index, (time, weights) in enumerate(frames):
            places = corners(doc, data, node_index, time, weights)
            low = [min(p[k] for p in places) for k in range(3)]
            high = [max(p[k] for p in places) for k in range(3)]
            mean = [sum(p[k] for p in places) / len(places) for k in range(3)]
            runs = runs_of(covered(places, camera, look))
            start = len(masks)
            masks += struct.pack(f"<{len(runs)}H", *[0xFFFF if r == 65536 else r for r in runs])
            print(folder, index, time, weights, low, high, mean, len(runs))
            vec = lambda v: "Vec3(" + ", ".join(number(x) for x in v) + ")"
            lines.append(
                f'    frames.Add(Worked("{folder}", "{names}", {index}, '
                f"{number(time) if time is not None else '-1'}, "
                f"{number(weights[0]) if weights is not None else '-1'}, "
                f"{vec(camera)}, {vec(look)},")
            lines.append(f"                      {vec(low)}, {vec(high)},")
            lines.append(f"                      {vec(mean)}, {start}, {len(runs)}))")
    lines += ["    Return frames", "End Function", ""]
    text = "\n".join(lines)
    if "--check" in sys.argv:
        # fb fmt lays worked_out.fb out, so it's the same but for its spaces.
        same = "".join(text.split()) == "".join((HERE / "worked_out.fb").read_text().split())
        if not same or bytes(masks) != (HERE / "masks.bin").read_bytes():
            sys.exit("worked_out.fb or masks.bin isn't what this works out: run it again")
        print("as worked out")
        return
    (HERE / "worked_out.fb").write_text(text, newline="\n")
    (HERE / "masks.bin").write_bytes(bytes(masks))


main()
