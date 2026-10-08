#!/usr/bin/env python3
"""Compare actual animation math with the original path and count pose fetches."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/code/z_skelanime.c").read_text()


def function(name):
    start = re.search(r"^\w+ " + name + r"\([^;]*?\)\s*\{", source, re.M).start()
    opening = source.index("{", start)
    end = source.index("\n}", opening) + 2
    return source[start:end] + "\n"


preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef float f32;
typedef int16_t s16;
typedef int32_t s32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef struct { s16 x, y, z; } Vec3s;
typedef struct { u16 x, y, z; } JointIndex;
typedef struct { JointIndex* jointIndices; s16* frameData; u16 staticIndexMax; } AnimationHeader;
typedef struct {
    AnimationHeader* animation;
    Vec3s *jointTable, *morphTable;
    s32 limbCount, mode;
    f32 curFrame, animLength, morphWeight, morphRate, playSpeed, startFrame, endFrame;
} SkelAnime;
#define SEGMENTED_TO_VIRTUAL(x) (x)
#define LOG_ADDRESS(...) ((void)0)
#define R_UPDATE_RATE 3
#define ANIM_INTERP 1
static unsigned fetches, blends;
'''
common = function("SkelAnime_GetFrameData").replace("s32 i;", "s32 i; fetches++;")
common += function("SkelAnime_InterpFrameTable").replace("s32 i;", "s32 i; blends++;")
animation = "".join(function(name) for name in ["SkelAnime_AnimateFrame", "SkelAnime_Once"])
reference = re.sub(r"\b(SkelAnime_AnimateFrame|SkelAnime_Once)\b", r"Reference_\1", animation)

tests = r'''
static uint32_t seed = 123;
static uint32_t next(void) { seed = seed * 1664525U + 1013904223U; return seed; }
static void check(AnimationHeader* header, unsigned i) {
    Vec3s actual[16], expected[16], morph[16];
    for (unsigned j = 0; j < 16; j++) {
        actual[j] = (Vec3s){next() >> 16, next() >> 16, next() >> 16};
        morph[j] = (Vec3s){next() >> 16, next() >> 16, next() >> 16};
    }
    memcpy(expected, actual, sizeof(actual));
    SkelAnime optimized = {
        .animation=header, .jointTable=actual, .morphTable=morph, .limbCount=i % 17,
        .mode=i % 2, .curFrame=(i % 32) + (i % 3) * 0.25f, .animLength=32,
        .morphWeight=(i % 5) * 0.25f, .morphRate=0.125f,
        .playSpeed=i % 3 == 0 ? -1 : i % 3 == 1 ? 0 : 1,
        .startFrame=0, .endFrame=31,
    };
    if (i % 7 == 0) optimized.curFrame = optimized.endFrame;
    SkelAnime original = optimized;
    original.jointTable = expected;
    if (i % 2) {
        assert(Reference_SkelAnime_Once(&original) == SkelAnime_Once(&optimized));
    } else {
        Reference_SkelAnime_AnimateFrame(&original);
        SkelAnime_AnimateFrame(&optimized);
    }
    assert(memcmp(actual, expected, sizeof(actual)) == 0);
    original.jointTable = actual;
    assert(memcmp(&original, &optimized, sizeof(original)) == 0);
}
int main(void) {
    JointIndex indices[16];
    s16 data[9 + 48 * 32];
    for (unsigned i = 0; i < sizeof(data)/sizeof(*data); i++) data[i] = next() >> 16;
    for (unsigned i = 0; i < 16; i++) {
        indices[i] = (JointIndex){i % 2 ? 9 + i * 96 : i % 9,
                                9 + i * 96 + 32, 9 + i * 96 + 64};
    }
    AnimationHeader header = {indices, data, 9};
    for (unsigned i = 0; i < 50000; i++) check(&header, i);
    Vec3s pose[16], morph[16] = {0};
    SkelAnime anim = {.animation=&header, .jointTable=pose, .morphTable=morph,
                     .limbCount=16, .mode=ANIM_INTERP, .curFrame=10, .animLength=32,
                     .endFrame=10};
    fetches = blends = 0;
    Reference_SkelAnime_AnimateFrame(&anim);
    assert(fetches == 2 && blends == 1);
    fetches = blends = 0;
    SkelAnime_AnimateFrame(&anim);
    assert(fetches == 1 && blends == 0);
    fetches = blends = 0;
    Reference_SkelAnime_Once(&anim);
    assert(fetches == 3 && blends == 1);
    fetches = blends = 0;
    SkelAnime_Once(&anim);
    assert(fetches == 1 && blends == 0);
    anim.curFrame = 10.5f;
    fetches = blends = 0;
    SkelAnime_AnimateFrame(&anim);
    assert(fetches == 2 && blends == 1);
    puts("Animation math: 50000 equivalence checks and pose-fetch checks passed");
}
'''

with tempfile.TemporaryDirectory(prefix="oot-animation-test-") as directory:
    cfile = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    cfile.write_text(preamble + common + "\n#define PLATFORM_PSP 0\n" + reference +
                     "\n#undef PLATFORM_PSP\n#define PLATFORM_PSP 1\n" + animation + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-fwrapv",
                    "-ffp-contract=off", "-Wall", "-Wextra", str(cfile), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
