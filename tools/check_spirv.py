"""Whether the engine's shaders do what a real GPU leaves undefined and lavapipe forgives, by reading
each built shader's SPIR-V, with no tool but this:

- A texture or sampler reached by an index that isn't a constant, without NonUniform on what it's
  reached through and everything made of it, as Vulkan asks. A GPU that needs it reads the wrong
  texture, or hangs; lavapipe never looks.
- A float turned straight into what indexes memory. Clamping it first says nothing of a NaN, and
  turning a NaN into an integer gives anything, so it has to be sorted out by a branch or a select,
  or a read through a device pointer goes wherever that says.

It checks itself on each run against examples of each kind, so it can't pass by finding nothing.

    python tools/check_spirv.py <build folder>
"""

import pathlib
import struct
import sys

OP_NAME = 5
OP_TYPE_ARRAY = 28
OP_TYPE_RUNTIME_ARRAY = 29
OP_TYPE_POINTER = 32
OP_CONSTANT_TRUE = 41
OP_CONSTANT_FALSE = 42
OP_CONSTANT = 43
OP_CONSTANT_COMPOSITE = 44
OP_CONSTANT_NULL = 46
OP_SPEC_CONSTANT = 50
OP_VARIABLE = 59
OP_LOAD = 61
OP_ACCESS_CHAIN = 65
OP_IN_BOUNDS_ACCESS_CHAIN = 66
OP_PTR_ACCESS_CHAIN = 67
OP_DECORATE = 71
OP_COPY_OBJECT = 83
OP_SAMPLED_IMAGE = 86
OP_IMAGE_WRITE = 99
OP_IMAGE = 100
OP_CONVERT_F_TO_U = 109
OP_CONVERT_F_TO_S = 110
OP_BITCAST = 124

STORAGE_UNIFORM_CONSTANT = 0
DECORATION_NON_UNIFORM = 5300

# What reads or asks about an image, whose first operand after its type and result is the image:
# the samples, the fetch, the gathers, the read; then the queries.
IMAGE_READS = {*range(87, 99), *range(101, 108), *range(305, 317)}
CONSTANTS = {OP_CONSTANT, OP_CONSTANT_NULL, OP_SPEC_CONSTANT, OP_CONSTANT_TRUE, OP_CONSTANT_FALSE,
             OP_CONSTANT_COMPOSITE}
CHAINS = {OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN, OP_PTR_ACCESS_CHAIN}
CONVERSIONS = {OP_CONVERT_F_TO_U, OP_CONVERT_F_TO_S}
# What an integer's made of that still has the conversion's value in it: conversions of integers,
# then arithmetic and bits. A phi or a select isn't here: that's what sorts a NaN out.
CARRIES = {OP_COPY_OBJECT, OP_BITCAST, 113, 114, *range(128, 140), *range(194, 201)}


def instructions(words):
    """Each instruction of a module, as its opcode and its operands."""
    at = 5
    while at < len(words):
        count = words[at] >> 16
        if count == 0 or at + count > len(words):
            raise ValueError(f"an instruction of {count} words at word {at} of {len(words)}")
        yield words[at] & 0xFFFF, words[at + 1:at + count]
        at += count


def words_of(data):
    if len(data) < 20 or len(data) % 4 != 0 or data[:4] != b"\x03\x02\x23\x07":
        raise ValueError("that isn't SPIR-V")
    return struct.unpack(f"<{len(data) // 4}I", data)


def name_in(operands):
    """The string an OpName holds, its words past the id."""
    text = struct.pack(f"<{len(operands) - 1}I", *operands[1:])
    return text.split(b"\0")[0].decode("utf-8", "replace")


def problems_of(words):
    """What's wrong with a module, a line each; and how many descriptors it reaches by an index."""
    listed = list(instructions(words))
    names = {}
    non_uniform = set()
    made = {}
    for opcode, operands in listed:
        if opcode == OP_NAME:
            names[operands[0]] = name_in(operands)
        elif opcode == OP_DECORATE and operands[1] == DECORATION_NON_UNIFORM:
            non_uniform.add(operands[0])
        elif opcode in CONSTANTS or opcode in CHAINS or opcode in CARRIES or opcode in CONVERSIONS:
            # Each of these has its type, then its result.
            made[operands[1]] = (opcode, operands[2:])

    def called(value):
        return f"{names[value]} ({value})" if value in names else str(value)

    pointers = {}
    arrays = set()
    variables = set()
    for opcode, operands in listed:
        if opcode == OP_TYPE_POINTER:
            pointers[operands[0]] = (operands[1], operands[2])
        elif opcode in (OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY):
            arrays.add(operands[0])
        elif opcode == OP_VARIABLE and operands[2] == STORAGE_UNIFORM_CONSTANT:
            pointer = pointers.get(operands[0])
            if pointer is not None and pointer[1] in arrays:
                variables.add(operands[1])

    problems = []

    def must_be_non_uniform(value, what):
        if value not in non_uniform:
            problems.append(f"{what} {called(value)} isn't NonUniform")

    # What's reached through an index, and all that's made of it.
    reached = set()
    indexed = 0
    for opcode, operands in listed:
        if opcode in CHAINS and operands[2] in variables:
            if made.get(operands[3], (None,))[0] in CONSTANTS:
                continue
            indexed += 1
            reached.add(operands[1])
            must_be_non_uniform(operands[1], f"an index into {called(operands[2])}: its pointer")
        elif opcode == OP_LOAD and operands[2] in reached:
            reached.add(operands[1])
            must_be_non_uniform(operands[1], "what's loaded through an index")
        elif opcode in (OP_SAMPLED_IMAGE, OP_IMAGE) and any(o in reached for o in operands[2:]):
            reached.add(operands[1])
            must_be_non_uniform(operands[1], "an image made of what's indexed")
        elif opcode in IMAGE_READS and operands[2] in reached:
            must_be_non_uniform(operands[2], f"the image of an operation {opcode}")
        elif opcode == OP_IMAGE_WRITE and operands[0] in reached:
            must_be_non_uniform(operands[0], "the image written")

    def conversion_in(value, seen):
        """Whether an integer has a float's conversion in it that nothing's sorted out."""
        if value in seen or value not in made:
            return False
        seen.add(value)
        opcode, parts = made[value]
        if opcode in CONVERSIONS:
            return True
        return opcode in CARRIES and any(conversion_in(part, seen) for part in parts)

    for opcode, operands in listed:
        if opcode in CHAINS:
            for index in operands[3:]:
                if conversion_in(index, set()):
                    problems.append(f"{called(operands[1])} indexes memory by a float's conversion "
                                    f"({called(index)}), which a NaN makes anything")
                    break
    return problems, indexed


# The examples below, made here so what they're of stays beside what looks at them.
def module(*body):
    words = [0x07230203, 0x00010500, 0, 100, 0]
    for instruction in body:
        words += [(len(instruction) << 16) | instruction[0], *instruction[1:]]
    return words


FLOAT = (22, 1, 32)
UINT = (21, 2, 32, 0)
IMAGE = (25, 3, 1, 1, 0, 0, 0, 1, 0)
ARRAY = (OP_TYPE_RUNTIME_ARRAY, 4, 3)
POINTER = (OP_TYPE_POINTER, 5, STORAGE_UNIFORM_CONSTANT, 4)
VARIABLE = (OP_VARIABLE, 5, 6, STORAGE_UNIFORM_CONSTANT)
DECLARED = (FLOAT, UINT, IMAGE, ARRAY, POINTER, VARIABLE)
CHAIN = (OP_ACCESS_CHAIN, 9, 10, 6, 7)
LOAD = (OP_LOAD, 3, 11, 10)
FETCH = (95, 8, 12, 11, 7)


def decorated(*values):
    return tuple((OP_DECORATE, value, DECORATION_NON_UNIFORM) for value in values)


FLOAT_INDEX = (
    (OP_CONVERT_F_TO_U, 2, 20, 21),
    (132, 2, 22, 20, 23),
    (128, 2, 24, 22, 25),
    (OP_PTR_ACCESS_CHAIN, 9, 26, 27, 24),
)
SORTED_INDEX = (
    (OP_CONVERT_F_TO_U, 2, 20, 21),
    (245, 2, 22, 20, 30, 23, 31),
    (OP_PTR_ACCESS_CHAIN, 9, 26, 27, 22),
)
EXAMPLES = (
    ("every part NonUniform", (*decorated(10, 11), *DECLARED, CHAIN, LOAD, FETCH), 0),
    ("a pointer without", (*decorated(11), *DECLARED, CHAIN, LOAD, FETCH), 1),
    ("a load without", (*decorated(10), *DECLARED, CHAIN, LOAD, FETCH), 2),
    ("none of it", (*DECLARED, CHAIN, LOAD, FETCH), 3),
    ("a sampled image without",
     (*decorated(10, 11), *DECLARED, CHAIN, LOAD, (OP_SAMPLED_IMAGE, 13, 14, 11, 15),
      (87, 8, 12, 14, 7)), 2),
    ("a constant index", (*DECLARED, (OP_CONSTANT, 2, 7, 0), CHAIN, LOAD, FETCH), 0),
    ("a float's conversion for an index", (*FLOAT_INDEX,), 1),
    ("one sorted out by a branch", (*SORTED_INDEX,), 0),
)


def check_itself():
    """Whether what this looks for, it finds, and what's fine, it leaves."""
    fine = True
    for name, body, wanted in EXAMPLES:
        found, _ = problems_of(module(*body))
        if len(found) != wanted:
            print(f"check_spirv's own example, {name}: {len(found)} found, not {wanted}: {found}")
            fine = False
    return fine


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    if not check_itself():
        return 1
    shaders = sorted((pathlib.Path(sys.argv[1]) / "native" / "fbengine" / "engine_shaders").glob("*.spv"))
    if not shaders:
        print(f"no shaders built in {sys.argv[1]}")
        return 1
    failed = False
    indexed = 0
    for shader in shaders:
        try:
            found, count = problems_of(words_of(shader.read_bytes()))
        except ValueError as error:
            print(f"{shader.name}: {error}")
            failed = True
            continue
        indexed += count
        for line in found:
            print(f"{shader.name}: {line}")
            failed = True
    # The engine draws through bindless, so none at all means this stopped seeing them.
    if indexed == 0:
        print("no shader reaches a texture by an index: this has stopped seeing them")
        failed = True
    print(f"looked at {len(shaders)} shaders' SPIR-V, {indexed} indexed textures and samplers")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
