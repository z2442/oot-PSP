#!/usr/bin/env python3
"""Exercise PSP high-pitch synthesis against an independently decimated stream."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    start = re.search(r"^(?:static (?:inline )?)?\w+\*? " + name + r"\([^;]*?\)\s*\{", source, re.M).start()
    return source[start:source.index("\n}", start) + 2] + "\n"


mixer = (ROOT / "src/port/psp/oot_psp_mixer.c").read_text()
playback = (ROOT / "src/audio/internal/playback.c").read_text()
synthesis = (ROOT / "src/audio/internal/synthesis.c").read_text()
audio = (ROOT / "include/audio.h").read_text()

preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
typedef int8_t s8;
typedef uint8_t u8;
typedef int16_t s16;
typedef uint16_t u16;
typedef int32_t s32;
typedef uint32_t u32;
typedef float f32;
typedef struct { u32 unused; } Acmd;
typedef s16 ADPCM_STATE[16];
typedef s16 RESAMPLE_STATE[16];
typedef struct { struct { u32 start, end, count; } header; s16 predictorState[16]; } AdpcmLoop;
typedef struct { struct { s32 order, numPredictors; } header; s16 book[32]; } AdpcmBook;
typedef struct { u8 codec, medium; u8* sampleAddr; AdpcmLoop* loop; AdpcmBook* book; } Sample;
typedef struct { Sample* sample; f32 tuning; } TunedSample;
#define UNUSED __attribute__((unused))
#define CLAMP_MAX(x, max) ((x) > (max) ? (max) : (x))
#define ROUND_UP_32(v) (((v) + 31) & ~31)
#define ROUND_UP_16(v) (((v) + 15) & ~15)
#define ROUND_UP_8(v) (((v) + 7) & ~7)
#define ROUND_DOWN_16(v) ((v) & ~15)
#define ALIGN16 ROUND_UP_16
#define ALIGN8 ROUND_UP_8
#define SAMPLE_SIZE 2
#define SAMPLES_PER_FRAME 16
#define DMEM_TEMP 0x3C0
#define DMEM_UNCOMPRESSED_NOTE 0x580
#define DMEM_PSP_HIGH_PITCH 0x1000
#define DMEM_COMPRESSED_ADPCM_DATA 0x940
#define DMEM_COMB_TEMP 0x760
#define A_INIT 1
#define A_CONTINUE 0
#define A_LOOP 2
#define MEDIUM_RAM 0
#define MEDIUM_UNK 1
#define CODEC_ADPCM 0
#define CODEC_S8 1
#define CODEC_S16_INMEMORY 2
#define CODEC_SMALL_ADPCM 3
#define CODEC_REVERB 4
#define CODEC_S16 5
#define HAAS_EFFECT_DELAY_NONE 0
#define HAAS_EFFECT_DELAY_LEFT 1
#define HAAS_EFFECT_DELAY_RIGHT 2
static struct {
    u16 in, out, nbytes;
    ADPCM_STATE* adpcmLoopState;
    union { u8 u8[0x2000]; s16 s16[0x1000]; } dmem;
} sMixer;
#define OOT_PSP_MIXER_STATE()
#define DMEM_U8(addr) (&sMixer.dmem.u8[(u16)(addr)])
#define DMEM_S16(addr) (&sMixer.dmem.s16[(u16)(addr) / 2])
'''
# Use the real note layouts, including the PSP flag and the N64 initializer ABI.
layouts = ""
for name in ("NoteSynthesisBuffers", "NoteSynthesisState", "NoteSubEu"):
    layouts += re.search(r"typedef struct " + name + r" \{.*?\} " + name + r";", audio, re.S)[0] + "\n"
state = r'''
typedef struct { u32 startSamplePos; NoteSubEu noteSubEu; } Note;
static struct { Note* notes; s16* curLoadedBook; struct { f32 resampleRate; } audioBufferParameters; } gAudioCtx;
static struct { bool buildingOnMe; } sOotPspAudioMeLocalFlags;
static s16 D_8012FBA8[32];
#define OotPspAudioSynth_IsAlignedNativePtr(ptr) ((ptr) != NULL)
#define OotPsp_IsLoadedNativeExternalAssetRange(ptr, size) false
#define OotPspAudioSynth_CanCacheSampleSpan(ptr, size) false
#define OotPspAudioSynth_DropBadNote(...) (assert(!"invalid note"), cmd)
#define AudioLoad_DmaSampleData(...) (assert(!"nonresident sample"), (u8*)0)
#define aLoadADPCM(...) assert(!"unexpected ADPCM")
#define aOotPspAudioLoadAdpcmCached(...) assert(!"unexpected ADPCM")
#define aADPCMdec(...) assert(!"unexpected ADPCM")
#define aLoadBuffer(pkt, src, dest, size) ((void)(pkt), OotPspMixer_LoadBuffer(src, dest, size))
#define aOotPspAudioLoadSampleCached aLoadBuffer
#define aSetBuffer(pkt, flags, in, out, size) ((void)(pkt), OotPspMixer_SetBuffer(flags, in, out, size))
#define AudioSynth_SetBuffer aSetBuffer
#define AudioSynth_ClearBuffer(pkt, dest, size) ((void)(pkt), OotPspMixer_ClearBuffer(dest, size))
#define aDMEMMove(pkt, in, out, size) ((void)(pkt), OotPspMixer_DMEMMove(in, out, size))
#define AudioSynth_DMemMove aDMEMMove
#define AudioSynth_InterL(pkt, in, out, count) ((void)(pkt), OotPspMixer_Interl(in, out, count))
#define AudioSynth_S8Dec(pkt, flags, state) ((void)(pkt), OotPspMixer_S8Dec(flags, state))
#define aSetLoop(pkt, state) ((void)(pkt), OotPspMixer_SetLoop((ADPCM_STATE*)(state)))
#define aResample(pkt, flags, pitch, state) ((void)(pkt), OotPspMixer_Resample(flags, pitch, state))
#define func_800DB2C0(...) ((void)0)
#define AudioSynth_UnkCmd19(...) assert(!"unexpected effect")
#define AudioSynth_UnkCmd3(...) assert(!"unexpected effect")
#define AudioSynth_HiLoGain(...) assert(!"unexpected effect")
#define AudioSynth_LoadFilterSize(...) assert(!"unexpected effect")
#define AudioSynth_LoadFilterBuffer(...) assert(!"unexpected effect")
#define AudioSynth_LoadBuffer(...) assert(!"unexpected effect")
#define AudioSynth_SaveBuffer(...) assert(!"unexpected effect")
#define AudioSynth_Mix(...) assert(!"unexpected effect")
#define AudioSynth_LoadWaveSamples(...) (assert(!"unexpected synthetic wave"), cmd)
#define AudioSynth_ProcessEnvelope(...) cmd
#define AudioSynth_ApplyHaasEffect(...) (assert(!"unexpected Haas effect"), cmd)
'''
actual_mixer = re.search(r"static s16 sResampleTable\[64\]\[4\] = \{.*?\n};", mixer, re.S)[0] + "\n"
for name in ("OotPspMixer_Clamp16", "OotPspMixer_ResampleProduct", "OotPspMixer_ResampleSample",
             "OotPspMixer_ClearBuffer", "OotPspMixer_LoadBuffer", "OotPspMixer_SetBuffer",
             "OotPspMixer_Interl", "OotPspMixer_DMEMMove", "OotPspMixer_SetLoop",
             "OotPspMixer_S8Dec", "OotPspMixer_Resample"):
    actual_mixer += function(mixer, name)
actual = function(playback, "Audio_NoteSetResamplingRate")
actual += function(synthesis, "AudioSynth_FinalResample")
actual += function(synthesis, "AudioSynth_ProcessNote")

tests = r'''
static u8 samples[65536] __attribute__((aligned(16)));
static unsigned checks;
static void check_rate(float sourceRate, float input) {
    gAudioCtx.audioBufferParameters.resampleRate = 32000.0f / sourceRate;
    /* Reuse state so the sweep also checks clearing the four-part flag. */
    static NoteSubEu sub;
    Audio_NoteSetResamplingRate(&sub, input);
    unsigned parts = sub.bitField0.hasFourParts ? 4 : sub.bitField1.hasTwoParts + 1;
    float expected = fminf(input, 3.99996f * 32000.0f / sourceRate);
    expected = fminf(expected, 7.99992f);
    float actual = sub.resamplingRateFixedPoint * (parts / 32768.0f);
    assert(fabsf(actual - expected) < parts / 32768.0f + 0.00001f);
    checks++;
}
static void check_stream(float input, unsigned end, unsigned loopStart, unsigned tickSize) {
    gAudioCtx.audioBufferParameters.resampleRate = 32000.0f / 22050.0f;
    memset(&sMixer, 0x5a, sizeof(sMixer));
    Note note = {0};
    NoteSynthesisBuffers buffers = {0};
    NoteSynthesisState synth = {.synthesisBuffers=&buffers};
    AdpcmLoop loop = {.header={loopStart, end, loopStart != 0}};
    for (unsigned i = 0; i < 16; i++) {
        loop.predictorState[i] = (s8)samples[(loopStart & ~15U) + i] * 256;
    }
    Sample sample = {.codec=CODEC_S8, .medium=MEDIUM_RAM, .sampleAddr=samples, .loop=&loop};
    TunedSample tuned = {&sample, 1.0f};
    NoteSubEu sub = {.tunedSample=&tuned};
    sub.bitField0.needsInit = true;
    gAudioCtx.notes = &note;
    Audio_NoteSetResamplingRate(&sub, input);
    assert(sub.bitField0.hasFourParts);
    RESAMPLE_STATE referenceState = {0};
    unsigned pos = 0;
    unsigned fraction = 0;
    Acmd commands[1000];
    for (unsigned tick = 0; tick < 20; tick++) {
        unsigned fixed = sub.resamplingRateFixedPoint * tickSize * 2 + fraction;
        unsigned count = fixed >> 16;
        fraction = fixed & 0xffff;
        s16 referenceInput[512] = {0};
        for (unsigned i = 0; i < count * 4; i++) {
            if (pos == end && loopStart) pos = loopStart;
            if (i % 4 == 0 && pos < end) referenceInput[i / 4] = (s8)samples[pos] * 256;
            pos++;
        }
        if (pos == end && loopStart) pos = loopStart;
        AudioSynth_ProcessNote(0, &sub, &synth, NULL, tickSize, commands, 0);
        s16 actual[208];
        memcpy(actual, DMEM_S16(DMEM_TEMP), tickSize * 2);
        /* Feed the complete decimated stream directly to the actual resampler.
         * This bypasses note splitting, decoding, staging and end-of-note clearing. */
        OotPspMixer_LoadBuffer(referenceInput, 0x1800, sizeof(referenceInput));
        OotPspMixer_SetBuffer(0, 0x1800, 0x1c00, tickSize * 2);
        OotPspMixer_Resample(tick == 0 ? A_INIT : A_CONTINUE, sub.resamplingRateFixedPoint, referenceState);
        if (memcmp(actual, DMEM_S16(0x1c00), tickSize * 2)) {
            fprintf(stderr, "PCM mismatch: pitch=%f end=%u loop=%u size=%u tick=%u count=%u pos=%u finished=%u\n",
                    input, end, loopStart, tickSize, tick, count, synth.samplePosInt, note.noteSubEu.bitField0.finished);
            for (unsigned i = 0; i < tickSize; i++) {
                if (actual[i] != DMEM_S16(0x1c00)[i]) {
                    fprintf(stderr, "sample %u actual=%d reference=%d\n", i, actual[i], DMEM_S16(0x1c00)[i]);
                    break;
                }
            }
            assert(false);
        }
        assert(synth.samplePosFrac == fraction);
        checks++;
        if (note.noteSubEu.bitField0.finished) break;
        assert(synth.samplePosInt == pos);
    }
}
int main(void) {
    for (unsigned i = 0; i < sizeof(samples); i++) samples[i] = (i * 73 + (i >> 3) * 19) % 255;
    /* Song of Storms D6; chest fanfare high strings/brass; rate boundaries. */
    for (unsigned rate = 22050; rate <= 44100; rate += 11025) {
        for (unsigned i = 0; i < 10000; i++) check_rate(rate, i / 1000.0f);
    }
    for (unsigned size = 8; size <= 192; size += 8) {
        for (unsigned r = 0; r < 20; r++) {
            float pitch = 4.0f + r * 0.095f;
            check_stream(pitch, 64000, 0, size);
            check_stream(pitch, 1237, 101, size);
            for (unsigned end = 1; end <= 257; end += 16) check_stream(pitch, end, 0, size);
        }
    }
    check_stream(4.104750f, 64000, 0, 192);
    puts("PSP audio pitch: rate, continuous/looping PCM, and short-note checks passed");
    printf("%u checks\n", checks);
}
'''
with tempfile.TemporaryDirectory(prefix="oot-audio-pitch-") as directory:
    cfile = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    cfile.write_text(preamble + "\n#define TARGET_PSP 1\n" + layouts + state +
                     "\n#undef TARGET_PSP\n" + actual_mixer + "\n#define TARGET_PSP 1\n" + actual + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-fwrapv", "-no-pie",
                    "-Wno-pointer-to-int-cast", "-Wno-int-to-pointer-cast", str(cfile), "-lm", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
