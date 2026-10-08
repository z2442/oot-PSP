#!/usr/bin/env python3
"""Compare actual PSP tracking code with the original path on the host.

Checks rotation/state equivalence and eliminated math calls. The PSP build
checks the real ABI; host checks do not measure hardware frame-time savings.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    start = re.search(r"^\w+ " + name + r"\(", source, re.M).start()
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


actor = (ROOT / "src/code/z_actor.c").read_text()
lib = (ROOT / "src/code/z_lib.c").read_text()
atan = (ROOT / "src/code/sys_math_atan.c").read_text()
tracking = actor[actor.index("typedef struct NpcTrackingRotLimits"):
                 actor.index("Gfx* func_80034B28(")]
symbols = ["NpcTrackingRotLimits", "NpcTrackingParams", "sNpcTrackingPresets", "Npc_TrackPointWithLimits",
           "Npc_GetTrackingPresetMaxPlayerYaw", "Npc_UpdateAutoTurn", "Npc_TrackPoint"]
reference = re.sub(r"\b(" + "|".join(symbols) + r")\b", r"Reference_\1", tracking)

preamble = r'''
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef float f32;
typedef int16_t s16;
typedef int32_t s32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef struct { f32 x, y, z; } Vec3f;
typedef struct { s16 x, y, z; } Vec3s;
typedef struct { struct { Vec3f pos; } world; struct { Vec3s rot; } shape; } Actor;
typedef struct {
    s16 talkState, trackingMode, autoTurnTimer, autoTurnState;
    Vec3s headRot, torsoRot;
    f32 yOffset;
    Vec3f trackPos;
} NpcInteractInfo;
enum { NPC_TRACKING_PLAYER_AUTO_TURN, NPC_TRACKING_NONE,
       NPC_TRACKING_HEAD_AND_TORSO, NPC_TRACKING_HEAD, NPC_TRACKING_FULL_BODY };
#define NPC_TALK_STATE_IDLE 0
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define SQ(x) ((x) * (x))
#define ABS(x) ((x) >= 0 ? (x) : -(x))
#define DECR(x) ((x) == 0 ? 0 : --(x))
#define CLAMP(x, min, max) ((x) < (min) ? (min) : (x) > (max) ? (max) : (x))
#define FALLTHROUGH __attribute__((fallthrough))
static unsigned sqrtCalls, atanCalls, smoothCalls;
static f32 counted_sqrtf(f32 x) { sqrtCalls++; return sqrtf(x); }
#define sqrtf counted_sqrtf
static uint32_t randState;
static s16 Rand_S16Offset(s16 base, s16 range) {
    randState = randState * 1664525U + 1013904223U;
    return base + randState % range;
}
'''
math = atan[atan.index("static u16 sAtan2Tbl"):
            atan.index("/**\n * @return angle to (x,y) from vector (1,0) around (0,0) in radians")]
math = math.replace("s32 ret;", "s32 ret; atanCalls++;")
for name in ["Math_Vec3f_DistXYZ", "Math_Vec3f_DistXZ", "Math_Vec3f_Yaw",
             "Math_Vec3f_Pitch", "Math_SmoothStepToS"]:
    math += function(lib, name)
math = math.replace("s16 stepSize = 0;", "s16 stepSize = 0; smoothCalls++;")

tests = r'''
static uint32_t inputState = 12345;
static uint32_t next(void) {
    inputState = inputState * 1664525U + 1013904223U;
    return inputState;
}
static f32 coordinate(void) { return (s16)(next() >> 16) / 8.0f; }
static void compare(Actor* actor, NpcInteractInfo* info, s16 preset, s16 mode) {
    Actor referenceActor = *actor;
    NpcInteractInfo referenceInfo = *info;
    uint32_t seed = next();
    randState = seed;
    Reference_Npc_TrackPoint(&referenceActor, &referenceInfo, preset, mode);
    uint32_t referenceRand = randState;
    randState = seed;
    Npc_TrackPoint(actor, info, preset, mode);
    assert(memcmp(actor, &referenceActor, sizeof(*actor)) == 0);
    assert(memcmp(info, &referenceInfo, sizeof(*info)) == 0);
    assert(randState == referenceRand);
}
int main(void) {
    Actor actor = {0};
    NpcInteractInfo info = {0};
    // Random poses, all presets/modes, coincident targets and yaw wraparound.
    for (unsigned i = 0; i < 200000; i++) {
        actor.world.pos = (Vec3f){coordinate(), coordinate(), coordinate()};
        actor.shape.rot = (Vec3s){0, next() >> 16, 0};
        info.trackPos = (Vec3f){coordinate(), coordinate(), coordinate()};
        if (i % 7 == 0) info.trackPos = actor.world.pos;
        if (i % 11 == 0) {
            actor.shape.rot.y = 0;
            info.trackPos = actor.world.pos;
            info.trackPos.z -= 100;
        }
        info.yOffset = coordinate();
        info.headRot = (Vec3s){next() >> 16, next() >> 16, 123};
        info.torsoRot = (Vec3s){next() >> 16, next() >> 16, -123};
        if (i % 3 == 0) info.headRot.x = info.headRot.y = info.torsoRot.x = info.torsoRot.y = 0;
        info.talkState = next() % 4;
        info.trackingMode = next() % 5;
        info.autoTurnState = next() % 4;
        info.autoTurnTimer = next() % 61;
        compare(&actor, &info, next() % ARRAY_COUNT(sNpcTrackingPresets), next() % 5);
    }
    // Multi-frame tracking, relaxation, and auto-turn sequences (including 170-unit boundary).
    for (s16 preset = 0; preset < ARRAY_COUNT(sNpcTrackingPresets); preset++) {
        for (s16 mode = 0; mode < 5; mode++) {
            memset(&actor, 0, sizeof(actor));
            memset(&info, 0, sizeof(info));
            info.yOffset = 40;
            for (unsigned frame = 0; frame < 1000; frame++) {
                f32 distance = frame % 3 == 0 ? nextafterf(170, 0) :
                               frame % 3 == 1 ? 170 : nextafterf(170, INFINITY);
                info.trackPos = (Vec3f){frame < 500 ? 0 : 50, 0, frame < 250 ? distance : -distance};
                info.talkState = frame >= 600 && frame < 650;
                compare(&actor, &info, preset, frame < 750 ? mode : NPC_TRACKING_NONE);
            }
        }
    }
    memset(&actor, 0, sizeof(actor));
    memset(&info, 0, sizeof(info));
    info.trackPos = (Vec3f){50, 10, 100};
    sqrtCalls = atanCalls = smoothCalls = 0;
    Reference_Npc_TrackPoint(&actor, &info, 1, NPC_TRACKING_HEAD_AND_TORSO);
    assert(sqrtCalls == 1 && atanCalls == 3 && smoothCalls == 4);
    sqrtCalls = atanCalls = smoothCalls = 0;
    Npc_TrackPoint(&actor, &info, 1, NPC_TRACKING_HEAD_AND_TORSO);
    assert(sqrtCalls == 1 && atanCalls == 2 && smoothCalls == 4);
    sqrtCalls = atanCalls = smoothCalls = 0;
    Npc_TrackPoint(&actor, &info, 1, NPC_TRACKING_NONE);
    assert(sqrtCalls == 0 && atanCalls == 1 && smoothCalls == 4);
    info.headRot.x = info.headRot.y = info.torsoRot.x = info.torsoRot.y = 0;
    sqrtCalls = atanCalls = smoothCalls = 0;
    Npc_TrackPoint(&actor, &info, 1, NPC_TRACKING_NONE);
    assert(sqrtCalls == 0 && atanCalls == 0 && smoothCalls == 0);
    puts("NPC tracking: 265000 equivalence checks and math-call checks passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="oot-npc-test-") as directory:
    source = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    source.write_text(preamble + math + "\n#define PLATFORM_PSP 0\n" + reference +
                      "\n#undef PLATFORM_PSP\n#define PLATFORM_PSP 1\n" + tracking + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-fwrapv",
                    "-ffp-contract=off", "-Wall", "-Wextra", "-Wno-sign-compare",
                    "-Wno-unused-variable", str(source), "-lm", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
