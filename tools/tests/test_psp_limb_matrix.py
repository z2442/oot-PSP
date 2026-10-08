#!/usr/bin/env python3
"""Interpret the actual VFPU helper instructions and compare limb transforms.

This checks register layout, branches, operation order, and matrix traffic.
PSPSDK compilation checks instruction encoding. Hardware testing is still
needed for VFPU timing and rendering; this is not a hardware emulator.
"""
from pathlib import Path
import random
import re
import struct

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/port/psp/oot_psp_vfpu_matrix.h").read_text()


def instructions(name):
    body = SOURCE.split("static inline void " + name + "(", 1)[1].split("\n}\n", 1)[0]
    return re.findall(r'^\s*"(.*?)\\n"', body, re.M)


def f32(value):
    return struct.unpack("f", struct.pack("f", value))[0]


class Vfpu:
    def __init__(self, matrix, translation, angles):
        self.registers = {}
        self.memory = {"mf": matrix.copy(), "translation": translation, "angles": angles}
        self.inputs = dict(zip(("x", "y", "z"), translation))
        self.inputs.update(rotateY=1, rotateX=1)
        self.matrix_accesses = 0

    def keys(self, name):
        bank, col, row = map(int, name[1:])
        if name[0] == "S":
            return [(bank, col, row)]
        if name[0] == "C":
            return [(bank, col, i) for i in range(4)]
        assert name[0] == "R", name
        return [(bank, i, row) for i in range(4)]

    def read(self, name):
        return [self.registers[key] for key in self.keys(name)]

    def write(self, name, values):
        for key, value in zip(self.keys(name), values):
            self.registers[key] = f32(value)

    def execute(self, code):
        labels = {line[:-1]: i for i, line in enumerate(code) if line.endswith(":")}
        pc = 0
        while pc < len(code):
            line = code[pc]
            pc += 1
            if line.endswith(":") or line == "nop":
                continue
            op, operands = line.split(" ", 1)
            args = operands.split(", ")
            if op in ("lv.s", "sv.s"):
                offset, address = re.fullmatch(r"(\d+)\(%\[(\w+)\]\)", args[1]).groups()
                index = int(offset) // 4
                if address == "mf":
                    self.matrix_accesses += 1
                if op == "lv.s":
                    self.write(args[0], [self.memory[address][index]])
                else:
                    self.memory[address][index] = self.read(args[0])[0]
            elif op == "mtv":
                self.write(args[1], [self.inputs[args[0][2:-1]]])
            elif op == "beqz":
                if self.inputs[args[0][2:-1]] == 0:
                    # These helpers explicitly use nop in every delay slot.
                    assert code[pc] == "nop"
                    pc = labels[args[1][:-1]]
            elif op == "vone.s":
                self.write(args[0], [1])
            elif op == "vmov.q":
                self.write(args[0], self.read(args[1]))
            elif op == "vscl.q":
                self.write(args[0], [f32(x * self.read(args[2])[0]) for x in self.read(args[1])])
            elif op in ("vadd.q", "vsub.q"):
                a, b = self.read(args[1]), self.read(args[2])
                self.write(args[0], [f32(x + y if op == "vadd.q" else x - y) for x, y in zip(a, b)])
            elif op == "vdot.q":
                products = [f32(x * y) for x, y in zip(self.read(args[1]), self.read(args[2]))]
                self.write(args[0], [f32(f32(f32(products[0] + products[1]) + products[2]) + products[3])])
            else:
                raise AssertionError(line)


translate = instructions("OotPspVfpu_MtxFTranslate")
rotate = [instructions("OotPspVfpu_MtxFRotate" + axis) for axis in "ZYX"]
combined = instructions("OotPspVfpu_MtxFTranslateRotateZYX")
table_source = (ROOT / "src/libultra/gu/sintable.inc.c").read_text()
table = [int(value, 0) for value in re.findall(r"0x[0-9a-fA-F]+|\b\d+\b", table_source.split("{", 1)[1].split("}", 1)[0])]
assert len(table) == 1024


def sin(angle):
    angle = (angle & 65535) >> 4
    value = table[1023 - (angle & 1023) if angle & 1024 else angle & 1023]
    return f32((-value if angle & 2048 else value) * f32(1 / 32767))


rng = random.Random(42)
for case in range(4000):
    # Include general matrices, negative scales, identity, and signed zero.
    matrix = [f32(rng.uniform(-10, 10)) for _ in range(16)]
    if case % 5 == 0:
        matrix = [1.0 if i % 5 == 0 else (-0.0 if case % 2 else 0.0) for i in range(16)]
    translation = [f32(rng.uniform(-3000, 3000)) for _ in range(3)]
    rotations = [rng.randrange(-32768, 32768) for _ in range(3)]
    # Check all combinations of zero/nonzero X/Y, plus zero Z and quadrant edges.
    if case % 4 < 2:
        rotations[1] = 0
    if case % 2 == 0:
        rotations[2] = 0
    if case % 11 == 0:
        rotations[0] = rng.choice([0, -32768, -16384, 16384, 32767])
    angles = [component for angle in rotations for component in (sin(angle), sin(angle + 16384))]
    original = Vfpu(matrix, translation, angles)
    original.execute(translate)
    for i in range(3):
        if i == 0 or rotations[i] != 0:
            original.inputs.update(sin=angles[2 * i], cos=angles[2 * i + 1])
            original.execute(rotate[i])
    optimized = Vfpu(matrix, translation, angles)
    optimized.inputs.update(rotateY=rotations[1] != 0, rotateX=rotations[2] != 0)
    optimized.execute(combined)
    assert struct.pack("16f", *original.memory["mf"]) == struct.pack("16f", *optimized.memory["mf"]), case
    assert optimized.matrix_accesses == 32
    assert original.matrix_accesses == 36 + 16 * ((rotations[1] != 0) + (rotations[2] != 0))
print("Limb matrices: 4000 instruction-model comparisons passed; matrix accesses 68 -> 32 with all axes")
