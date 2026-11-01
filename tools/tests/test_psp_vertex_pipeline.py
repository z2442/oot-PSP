#!/usr/bin/env python3
"""Interpret actual PSP transform/clip assembly to check the 64-byte vertex ABI.

This small instruction interpreter checks memory traffic, normals, branches and
interpolation. PSPSDK checks instruction encoding; GE output still needs a PSP.
"""
from pathlib import Path
import math
import random
import re
import struct

ROOT = Path(__file__).resolve().parents[2] / "src/port/psp/gfx"


def bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def floating(value):
    return struct.unpack("<f", struct.pack("<I", value & 0xffffffff))[0]


def signed(value):
    return (value & 0x7fffffff) - (value & 0x80000000)


class Machine:
    def __init__(self):
        self.memory = bytearray(0x10000)
        self.reg = {"$zero": 0, "$ra": 0xffffffff}
        self.vfpu = {}
        self.fpu = {}
        self.condition = False

    def store(self, address, fmt, *values):
        struct.pack_into("<" + fmt, self.memory, address, *values)

    def load(self, address, fmt):
        return struct.unpack_from("<" + fmt, self.memory, address)

    def keys(self, name, count):
        bank, col, row = map(int, name[1:])
        assert name[0] in "SC"
        return [(bank, col, row + i) for i in range(count)]

    def vector(self, name, count):
        return [floating(self.vfpu[key]) for key in self.keys(name, count)]

    def put_vector(self, name, values):
        for key, value in zip(self.keys(name, len(values)), values):
            self.vfpu[key] = bits(value)

    def execute(self, name, arguments):
        text = (ROOT / name).read_text()
        definitions = dict(re.findall(r"^#define\s+(\w+)\s+(\S+)", text, re.M))
        code = []
        labels = {}
        for line in re.sub(r"/\*.*?\*/", "", text, flags=re.S).splitlines():
            line = line.strip()
            if not line or line[0] in ".#" and not line.endswith(":"):
                continue
            if line.endswith(":"):
                labels[line[:-1]] = len(code)
            else:
                code.append(line)
        for i, value in enumerate(arguments):
            self.reg[f"$a{i}"] = value

        def immediate(value):
            return int(definitions.get(value, value), 0)

        def instruction(line):
            if line == "nop":
                return None
            op, operand = line.split(None, 1)
            args = [a.strip() for a in operand.split(",")]
            base = op.split(".")[0]
            if op in ("b", "beq", "bne", "bvt", "jr"):
                if op == "jr":
                    return -1
                take = op == "b" or op == "bvt" and self.condition
                if op in ("beq", "bne"):
                    take = (self.reg[args[0]] == self.reg[args[1]]) == (op == "beq")
                return labels[args[-1]] if take else None
            if op in ("lb", "lbu", "lh", "lw", "sb", "sw", "lv.q", "lv.s", "sv.q", "sv.s"):
                offset, register = re.fullmatch(r"(\w+)\((\$\w+)\)", args[1]).groups()
                address = self.reg[register] + immediate(offset)
                if op.startswith(("lv.", "sv.")):
                    count = 4 if op.endswith("q") else 1
                    for i, key in enumerate(self.keys(args[0], count)):
                        if op.startswith("lv"):
                            self.vfpu[key] = self.load(address + 4*i, "I")[0]
                        else:
                            self.store(address + 4*i, "I", self.vfpu[key])
                elif op in ("sb", "sw"):
                    self.store(address, "B" if op == "sb" else "I", self.reg[args[0]] & (255 if op == "sb" else 0xffffffff))
                else:
                    self.reg[args[0]] = self.load(address, {"lb": "b", "lbu": "B", "lh": "h", "lw": "I"}[op])[0] & 0xffffffff
            elif op in ("li", "or", "addu", "addiu", "sll", "srl", "andi"):
                if op == "li":
                    value = immediate(args[1])
                else:
                    a = self.reg[args[1]]
                    b = self.reg[args[2]] if op in ("or", "addu") else immediate(args[2])
                    value = {"or": lambda: a | b, "addu": lambda: a+b, "addiu": lambda: a+b,
                             "sll": lambda: a << b, "srl": lambda: a >> b, "andi": lambda: a & b}[op]()
                self.reg[args[0]] = value & 0xffffffff
            elif op in ("mtv", "mfv", "mtc1", "mfc1"):
                if op == "mtv": self.vfpu[self.keys(args[1], 1)[0]] = self.reg[args[0]]
                if op == "mfv": self.reg[args[0]] = self.vfpu[self.keys(args[1], 1)[0]]
                if op == "mtc1": self.fpu[args[1]] = self.reg[args[0]]
                if op == "mfc1": self.reg[args[0]] = self.fpu[args[1]]
            elif op in ("cvt.s.w", "trunc.w.s", "sub.s", "mul.s", "add.s"):
                a = floating(self.fpu[args[1]])
                if op == "cvt.s.w": value = bits(signed(self.fpu[args[1]]))
                elif op == "trunc.w.s": value = math.trunc(a) & 0xffffffff
                else:
                    b = floating(self.fpu[args[2]])
                    value = bits({"sub.s": lambda: a-b, "mul.s": lambda: a*b, "add.s": lambda: a+b}[op]())
                self.fpu[args[0]] = value
            elif base.startswith("v"):
                count = {"s": 1, "p": 2, "t": 3, "q": 4}[op.split(".")[1]]
                if base == "vi2f":
                    self.put_vector(args[0], [signed(self.vfpu[k]) / 2**immediate(args[2]) for k in self.keys(args[1], count)])
                elif base == "vf2iz":
                    for key, value in zip(self.keys(args[0], count), self.vector(args[1], count)):
                        self.vfpu[key] = math.trunc(value * 2**immediate(args[2])) & 0xffffffff
                elif base == "vmov":
                    values = [self.vfpu[k] for k in self.keys(args[1], count)]
                    for key, value in zip(self.keys(args[0], count), values): self.vfpu[key] = value
                elif base in ("vzero", "vone"):
                    self.put_vector(args[0], [float(base == "vone")] * count)
                elif base == "vcmp":
                    a, b = self.vector(args[1], 1)[0], self.vector(args[2], 1)[0]
                    self.condition = {"GT": a > b, "LE": a <= b}[args[0]]
                elif base == "vdot":
                    self.put_vector(args[0], [sum(a*b for a, b in zip(self.vector(args[1], count), self.vector(args[2], count)))])
                elif base == "vrcp":
                    self.put_vector(args[0], [1 / value for value in self.vector(args[1], count)])
                else:
                    a = self.vector(args[1], count)
                    b = self.vector(args[2], 1)*count if base == "vscl" else self.vector(args[2], count)
                    operations = {"vsub": lambda x,y: x-y, "vadd": lambda x,y: x+y,
                                  "vmul": lambda x,y: x*y, "vscl": lambda x,y: x*y,
                                  "vmax": max, "vmin": min}
                    self.put_vector(args[0], [operations[base](x,y) for x, y in zip(a,b)])
            else:
                raise AssertionError(line)
            return None

        pc = 0
        steps = 0
        while 0 <= pc < len(code):
            steps += 1
            assert steps < 100000
            line = code[pc]
            target = instruction(line)
            if line.split()[0] in ("b", "beq", "bne", "bvt", "jr"):
                instruction(code[pc+1])
                pc = target if target is not None else pc+2
            else:
                pc += 1


rng = random.Random(404)
for lighting in (0, 1):
    for count in (1, 3, 64):
        m = Machine()
        model = [rng.uniform(-2, 2) for _ in range(16)]
        projection = [float(i % 5 == 0) for i in range(16)]
        model[3] = model[7] = model[11] = 0
        model[15] = 1
        m.store(0x4000, "16f", *model)
        m.store(0x4100, "16f", *projection)
        m.store(0x3000, "IIiiII", 0x4000, 0x4100, 32, 64, 1, lighting)
        inputs = []
        for i in range(count):
            position = [rng.randrange(-20, 20) for _ in range(3)]
            normal = [rng.randrange(-128, 128) for _ in range(3)]
            inputs.append((position, normal))
            m.store(0x2000 + i*16, "hhhHhhbbbB", *position, 0, 0, 0, *normal, 37+i)
        m.execute("gfx_transform_vfpu.s", (0x1000, 0x2000, count, 0x3000))
        for i, (position, normal) in enumerate(inputs):
            actual = m.load(0x1000+i*64, "16f")
            expected = [sum(position[k]*model[k*4+j] for k in range(3)) + model[12+j] for j in range(4)]
            for j in range(4): assert abs(actual[j]-expected[j]) < 0.0001
            for j in range(3):
                expected_normal = sum(normal[k]*model[k*4+j] for k in range(3)) if lighting else 0
                assert abs(actual[12+j]-expected_normal) < 0.0001
            assert m.load(0x1000+i*64+43, "B")[0] == 37+i
            assert abs(m.load(0x1000+i*64+45, "B")[0] - max(0, min(255, int(expected[2]*32+64)))) <= 1

# Clip a triangle against a plane: intersections must carry interpolated normals,
# RGBA, UV and fog independently, with a bit-exact integer lighting snapshot.
for trial in range(100):
    m = Machine()
    vertices = []
    for i, x in enumerate((2, -0.5, -0.5)):
        vertex = [x, -0.5 if i != 2 else 0.5, 0, 1]
        normal = [rng.uniform(-2, 2) for _ in range(3)]
        rgba = [rng.randrange(256) for _ in range(4)]
        fog = rng.randrange(256)
        uv = [rng.uniform(-10, 10) for _ in range(2)]
        m.store(0x7000 + 64*i, "10f4BBBH3fI", *vertex, *vertex, *uv, *rgba, 0, fog, 0, *normal, 17)
        vertices.append((vertex, uv, rgba, fog, normal))
    m.store(0x9000, "4f", 1, 0, 0, -1)
    m.execute("gfx_clip_vfpu.s", (0x8000, 0x7000, 0x9000, 3))
    assert m.reg["$v0"] == 4
    a, b, c = vertices
    # Assembly traverses edges a->b, b->c, c->a, emitting intersections first.
    for i, (left, right, factor) in enumerate(((a,b,0.4),(b,b,0),(c,c,0),(c,a,0.6))):
        out = 0x8000 + 64*i
        normal = m.load(out+48, "3f")
        for j in range(3): assert abs(normal[j] - (left[4][j] + (right[4][j]-left[4][j])*factor)) < 0.00001
        rgba = m.load(out+40, "4B")
        for j in range(4): assert abs(rgba[j] - int(left[2][j]+(right[2][j]-left[2][j])*factor)) <= 1
        assert abs(m.load(out+45, "B")[0] - int(left[3]+(right[3]-left[3])*factor)) <= 1
        assert m.load(out+60, "I")[0] == 17

print("PSP vertex assembly: limb normals, 64-byte stride, alpha/fog and 100 clipped triangles passed")
