"""Works out, from the glTF specification, with none of the engine's code, where each of the sample
models' frames should be: its vertices' box and mean in the world, over its triangles' corners,
exactly which pixels they cover, and how a few of them are lit, by glTF's BRDF. All it takes of the
engine is how its camera sees (60 degrees up its view, down its -Z), what it names an unnamed node,
what its ambient light does (lights the base color, from everywhere), how a point light falls off
(the inverse square, windowed to 0 at its range), and that a file with no normals gets its
triangles' own, flat, before it's posed. It writes worked_out.fb and masks.bin, which main.fb
compares what the engine does with. Run it again when FRAMES changes:

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

# Its folder, its file, the mesh's node, then for each frame, the time to pose a clip at, or None for
# none, the weights to set, or None for its own, and which clip, its first if it isn't said; and the
# camera's place and where it looks.
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
    # Normals that its morph targets move: at rest, mostly thin, between both, and mostly a ramp.
    ("AnimatedMorphCube", "AnimatedMorphCube.glb", 0, [(t, None) for t in (0.0, 1.0, 2.1, 3.3)],
     (3, 2.5, 4), (0, 0, 0)),
    # A character of three clips, a skin of 24 joints and no indices or normals, from its side: at
    # rest, and twice in each of Survey, Walk, and Run, none of them on a key.
    ("Fox", "Fox.glb", 1,
     [(None, None), (1.1, None, 0), (2.6, None, 0), (0.3, None, 1), (0.6, None, 1), (0.3, None, 2),
      (0.9, None, 2)],
     (200, 45, -10), (0, 38, -10)),
]

# main.fb's light: white, at the camera's place plus this, out to its range, and its ambient light.
LIGHT_OFF = (3.0, 4.0, 0.0)
# Its intensity, a folder's or else the rest's: enough that the light shows, past the ambient's,
# on a rough metal, glTF's default, and on RiggedSimple's far plastic, and little enough that
# MorphPrimitivesTest's red isn't clipped.
INTENSITY = {"SimpleSkin": 150.0, "SimpleMorph": 150.0, "RiggedSimple": 400.0, "AnimatedMorphCube": 100.0,
             "Fox": 30000.0}
INTENSITY_ELSE = 15.0
# How far it reaches, a folder's or else the rest's: Fox is 170 long, and its camera 200 away.
RANGES = {"Fox": 400.0}
RANGE_ELSE = 100.0
AMBIENT = 0.3
# glTF's material when a primitive has none.
DEFAULT_MATERIAL = {"baseColorFactor": [1.0, 1.0, 1.0, 1.0], "metallicFactor": 1.0, "roughnessFactor": 1.0}
# How many lit pixels a frame's checked at, each this many pixels or more inside its triangle.
LIT_PIXELS = 5
MARGIN = 0.3
# How near a triangle's edge a pixel's middle is, in pixels, to be on it: a rasterizer puts each
# corner on a grid of 1/256 of a pixel, lavapipe's, and the GPU's floats round where its skin puts
# it, by about 1/1000 of a pixel of Fox's, so whether one so near is covered could go either way.
EDGE = 1 / 128

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


def pose(doc, data, time, clip=0):
    """Each node's local parts, and its weights, as its animation `clip` has them at `time`."""
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
    animation = doc["animations"][clip]
    for channel in animation["channels"]:
        sampler = animation["samplers"][channel["sampler"]]
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


def turned(m, n):
    """A normal by `m`'s 3 by 3, its inverse's transpose, as cofactors: what keeps it at right angles."""
    a = (m[0], m[1], m[2])
    b = (m[4], m[5], m[6])
    c = (m[8], m[9], m[10])
    cross = lambda u, v: (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    bc, ca, ab = cross(b, c), cross(c, a), cross(a, b)
    made = [bc[k] * n[0] + ca[k] * n[1] + ab[k] * n[2] for k in range(3)]
    if sum(x * y for x, y in zip(a, bc)) < 0:
        made = [-x for x in made]
    return unit(made)


def unit(v):
    size = math.sqrt(sum(x * x for x in v))
    return tuple(x / size for x in v) if size > 0 else (0.0, 0.0, 0.0)


def flat(positions, indices):
    """Each corner's triangle's own normal, by its winding, as a file with no normals gets."""
    made = []
    for t in range(0, len(indices), 3):
        a, b, c = (positions[i] for i in indices[t:t + 3])
        u = [y - x for x, y in zip(a, b)]
        v = [y - x for x, y in zip(a, c)]
        made += [unit((u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]))] * 3
    return made


def material_of(doc, primitive):
    found = dict(DEFAULT_MATERIAL)
    if "material" in primitive:
        found.update(doc["materials"][primitive["material"]].get("pbrMetallicRoughness", {}))
        found.setdefault("metallicFactor", 1.0)
    return found


def corners(doc, data, node_index, time, weights, clip=0):
    """Every triangle corner's place in the world."""
    return [c[0] for c in lit_corners(doc, data, node_index, time, weights, clip)]


def lit_corners(doc, data, node_index, time, weights, clip=0):
    """Every triangle corner's place in the world, its normal there, and its primitive's material
    without its texture, which main.fb takes off to draw the frame again for its light."""
    parts = pose(doc, data, time, clip)
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
        # With no indices, each three vertices are a triangle, as Fox's are.
        if "indices" in primitive:
            indices = [i[0] for i in read(doc, data, primitive["indices"])]
        else:
            indices = list(range(len(positions)))
        material = {k: v for k, v in material_of(doc, primitive).items() if k != "baseColorTexture"}
        # A file's normals by its vertices; flat ones by the corners, as they take them apart.
        has_normals = "NORMAL" in primitive["attributes"]
        normals = [list(n) for n in read(doc, data, primitive["attributes"]["NORMAL"])] if has_normals else None
        moved = [list(p) for p in positions]
        for t, target in enumerate(primitive.get("targets", [])):
            if t < len(used) and "POSITION" in target:
                for i, by in enumerate(read(doc, data, target["POSITION"])):
                    for k in range(3):
                        moved[i][k] += by[k] * used[t]
            if t < len(used) and "NORMAL" in target and normals is not None:
                for i, by in enumerate(read(doc, data, target["NORMAL"])):
                    for k in range(3):
                        normals[i][k] += by[k] * used[t]
        rest = None if has_normals else flat(positions, indices)
        final = []
        matrices = []
        if palette is not None:
            joints = read(doc, data, primitive["attributes"]["JOINTS_0"])
            shares = read(doc, data, primitive["attributes"]["WEIGHTS_0"])
            for i, p in enumerate(moved):
                blended = [0.0] * 16
                for k in range(4):
                    if shares[i][k] != 0.0:
                        blended = [a + b * shares[i][k] for a, b in zip(blended, palette[joints[i][k]])]
                final.append(point(blended, p))
                matrices.append(blended)
        else:
            final = [point(world[node_index], p) for p in moved]
            matrices = [world[node_index]] * len(moved)
        for c, i in enumerate(indices):
            normal = normals[i] if normals is not None else rest[c]
            placed.append((final[i], turned(matrices[i], normal), material))
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


def on_edges(places, camera, look):
    """The pixels, by y * SIZE + x, whose middles are within EDGE of a triangle's edge, and inside no
    triangle by more."""
    right, above, back = view_of(camera, look)
    per = (SIZE / 2) / math.tan(FIELD / 2)
    seen = []
    for p in places:
        d = [a - c for a, c in zip(p, camera)]
        depth = -sum(a * b for a, b in zip(d, back))
        seen.append((SIZE / 2 + sum(a * b for a, b in zip(d, right)) * per / depth,
                     SIZE / 2 - sum(a * b for a, b in zip(d, above)) * per / depth))
    inside = {}
    for t in range(0, len(seen), 3):
        (ax, ay), (bx, by), (cx, cy) = seen[t:t + 3]
        twice = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay)
        if twice == 0.0:
            continue
        edges = [math.hypot(bx - cx, by - cy), math.hypot(cx - ax, cy - ay), math.hypot(ax - bx, ay - by)]
        for y in range(max(0, math.floor(min(ay, by, cy)) - 1), min(SIZE, math.ceil(max(ay, by, cy)) + 2)):
            py = y + 0.5
            for x in range(max(0, math.floor(min(ax, bx, cx)) - 1), min(SIZE, math.ceil(max(ax, bx, cx)) + 2)):
                px = x + 0.5
                one = ((bx - px) * (cy - py) - (cx - px) * (by - py)) / twice
                two = ((cx - px) * (ay - py) - (ax - px) * (cy - py)) / twice
                # How far inside it is, or outside for below 0, from its nearest edge.
                far = min(b * abs(twice) / e for b, e in zip((one, two, 1 - one - two), edges) if e > 0)
                inside[(x, y)] = max(inside.get((x, y), far), far)
    return sorted(y * SIZE + x for (x, y), far in inside.items() if abs(far) < EDGE)


def view_of(camera, look):
    """The camera's right, up, and back, as main.fb's camera points."""
    back = unit([c - l for c, l in zip(camera, look)])
    up = (0.0, 1.0, 0.0)
    right = unit([up[1] * back[2] - up[2] * back[1], up[2] * back[0] - up[0] * back[2],
                  up[0] * back[1] - up[1] * back[0]])
    above = (back[1] * right[2] - back[2] * right[1], back[2] * right[0] - back[0] * right[2],
             back[0] * right[1] - back[1] * right[0])
    return right, above, back


def encoded(linear):
    """A linear value 0 to 1 as an sRGB target stores it."""
    v = min(max(linear, 0.0), 1.0)
    v = v * 12.92 if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055
    return round(v * 255)


def brdf(material, normal, view, to_light, radiance):
    """glTF's metal-rough BRDF, as its specification and Khronos's viewer have it: Lambert's diffuse,
    GGX's specular, Schlick's Fresnel, and Smith's height-correlated visibility."""
    dot = lambda u, v: sum(x * y for x, y in zip(u, v))
    n_l = max(dot(normal, to_light), 0.0)
    if n_l <= 0.0:
        return [0.0, 0.0, 0.0]
    halfway = unit([a + b for a, b in zip(view, to_light)])
    n_v = min(max(dot(normal, view), 0.0), 1.0)
    n_h = min(max(dot(normal, halfway), 0.0), 1.0)
    v_h = min(max(dot(view, halfway), 0.0), 1.0)
    base = material["baseColorFactor"][:3]
    metal = material["metallicFactor"]
    alpha = max(material.get("roughnessFactor", 1.0) ** 2, 0.045 ** 2)
    a2 = alpha * alpha
    d = a2 / (math.pi * (n_h * n_h * (a2 - 1) + 1) ** 2)
    ggx_v = n_l * math.sqrt(n_v * n_v * (1 - a2) + a2)
    ggx_l = n_v * math.sqrt(n_l * n_l * (1 - a2) + a2)
    visibility = 0.5 / (ggx_v + ggx_l) if ggx_v + ggx_l > 0 else 0.0
    made = []
    for k in range(3):
        f0 = 0.04 + (base[k] - 0.04) * metal
        fresnel = f0 + (1 - f0) * (1 - v_h) ** 5
        diffuse = (1 - fresnel) * base[k] * (1 - metal) / math.pi
        made.append((diffuse + fresnel * visibility * d) * radiance * n_l)
    return made


def lit(placed, camera, look, intensity, reach):
    """A few pixels well inside the nearest triangle there, front on, each with the color glTF's BRDF
    gives it there: x, y, red, green, blue."""
    right, above, back = view_of(camera, look)
    per = (SIZE / 2) / math.tan(FIELD / 2)
    light = [c + o for c, o in zip(camera, LIGHT_OFF)]
    nearest = {}
    for t in range(0, len(placed), 3):
        three = placed[t:t + 3]
        seen = []
        for place, _, _ in three:
            d = [a - c for a, c in zip(place, camera)]
            depth = -sum(a * b for a, b in zip(d, back))
            seen.append((SIZE / 2 + sum(a * b for a, b in zip(d, right)) * per / depth,
                         SIZE / 2 - sum(a * b for a, b in zip(d, above)) * per / depth, depth))
        (ax, ay, az), (bx, by, bz), (cx, cy, cz) = seen
        twice = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay)
        # Counter-clockwise in the world is clockwise here, as y runs down: those face the camera.
        if twice >= 0.0:
            continue
        edges = [math.hypot(bx - cx, by - cy), math.hypot(cx - ax, cy - ay), math.hypot(ax - bx, ay - by)]
        for y in range(max(0, math.floor(min(ay, by, cy))), min(SIZE, math.ceil(max(ay, by, cy)) + 1)):
            py = y + 0.5
            for x in range(max(0, math.floor(min(ax, bx, cx))), min(SIZE, math.ceil(max(ax, bx, cx)) + 1)):
                px = x + 0.5
                one = ((bx - px) * (cy - py) - (cx - px) * (by - py)) / twice
                two = ((cx - px) * (ay - py) - (ax - px) * (cy - py)) / twice
                three_ = 1 - one - two
                if min(one, two, three_) < 0:
                    continue
                # Perspective's: by one over depth, then made whole.
                w = [one / az, two / bz, three_ / cz]
                depth = 1 / sum(w)
                margin = min(abs(twice) * b / e for b, e in zip((one, two, three_), edges))
                if (x, y) not in nearest or depth < nearest[(x, y)][0]:
                    nearest[(x, y)] = (depth, margin, [v * depth for v in w], three)
    candidates = sorted(((m, xy) for xy, (_, m, _, _) in nearest.items() if m >= MARGIN), key=lambda c: (-c[0], c[1]))
    chosen = []
    for _, xy in candidates:
        if all(math.hypot(xy[0] - o[0], xy[1] - o[1]) > 12 for o in chosen):
            chosen.append(xy)
        if len(chosen) == LIT_PIXELS:
            break
    made = []
    for x, y in chosen:
        _, _, w, three = nearest[(x, y)]
        place = [sum(w[i] * three[i][0][k] for i in range(3)) for k in range(3)]
        normal = unit([sum(w[i] * three[i][1][k] for i in range(3)) for k in range(3)])
        material = three[0][2]
        to_light = [l - p for l, p in zip(light, place)]
        apart = math.sqrt(sum(v * v for v in to_light))
        window = min(max(1 - (apart / reach) ** 4, 0.0), 1.0)
        radiance = intensity * window * window / apart ** 2
        shaded = brdf(material, normal, unit([c - p for c, p in zip(camera, place)]), unit(to_light), radiance)
        base = material["baseColorFactor"][:3]
        made.append((x, y, *(encoded(base[k] * AMBIENT + shaded[k]) for k in range(3))))
    return made


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
             "    // Which of its clips Time is in, by its place in the file.", "    Package Field Clip: Int",
             "    Package Field Camera: Vec3", "    Package Field Look: Vec3", "    Package Field Low: Vec3",
             "    Package Field High: Vec3", "    Package Field Mean: Vec3",
             "    // Where its mask's runs start in masks.bin, and how many.", "    Package Field Runs: Int",
             "    Package Field RunCount: Int",
             "    // Its light's intensity and range, and pixels it lights, each x, y, red, green, and blue, as",
             "    // stored.",
             "    Package Field Intensity: Float", "    Package Field Range: Float", "    Package Field Lit: Int[]",
             "    // Pixels, by y * 256 + x, on an edge, which its mask leaves out.", "    Package Field Edges: Int[]",
             "End Struct", "", "// Time and Weight are below 0 for none.", "Package Function WorkedOut(): Worked[]",
             "    Local frames: Worked[] = []"]
    for folder, file, node_index, frames, camera, look in FRAMES:
        doc, data = load(HERE / folder / file)
        names = doc["nodes"][node_index].get("name", f"node_{node_index}")
        for index, frame in enumerate(frames):
            time, weights, clip = (*frame, 0)[:3]
            placed = lit_corners(doc, data, node_index, time, weights, clip)
            places = [c[0] for c in placed]
            intensity = INTENSITY.get(folder, INTENSITY_ELSE)
            reach = RANGES.get(folder, RANGE_ELSE)
            shown = [v for pixel in lit(placed, camera, look, intensity, reach) for v in pixel]
            low = [min(p[k] for p in places) for k in range(3)]
            high = [max(p[k] for p in places) for k in range(3)]
            mean = [sum(p[k] for p in places) / len(places) for k in range(3)]
            runs = runs_of(covered(places, camera, look))
            edges = on_edges(places, camera, look)
            start = len(masks)
            masks += struct.pack(f"<{len(runs)}H", *[0xFFFF if r == 65536 else r for r in runs])
            print(folder, index, time, weights, low, high, mean, len(runs), shown, len(edges), "on edges")
            vec = lambda v: "Vec3(" + ", ".join(number(x) for x in v) + ")"
            lines.append(
                f'    frames.Add(Worked("{folder}", "{names}", {index}, '
                f"{number(time) if time is not None else '-1'}, "
                f"{number(weights[0]) if weights is not None else '-1'}, {clip}, "
                f"{vec(camera)}, {vec(look)},")
            lines.append(f"                      {vec(low)}, {vec(high)},")
            lines.append(f"                      {vec(mean)}, {start}, {len(runs)}, {number(intensity)}, "
                         f"{number(reach)},")
            lines.append(f"                      [{', '.join(str(v) for v in shown)}] As Int[],")
            lines.append(f"                      [{', '.join(str(v) for v in edges)}] As Int[]))")
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
