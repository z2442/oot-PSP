#!/usr/bin/env python3
"""Run the real video handoff/presentation with threaded PSP kernel stubs."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
backend = (ROOT / "src/port/psp/gfx/gfx_scegu.c").read_text()
worker = (ROOT / "src/port/psp/oot_psp_video_thread.c").read_text()
renderer = (ROOT / "src/port/psp/gfx/gfx_fast3d.c").read_text()


def function(source, name):
    start = re.search(r"^(?:static )?[^\n]*\b" + name + r"\([^;]*?\)\s*\{", source, re.M).start()
    return source[start:source.index("\n}", start) + 2] + "\n"


stubs = r'''
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TARGET_PSP 1
#define F3DEX_GBI_2 1
#define OOT_PSP_NATIVE_MESH_ENTRIES 2
static struct { bool pinned; } sNativeMeshes[OOT_PSP_NATIVE_MESH_ENTRIES];
static unsigned sTextureCacheFrameSerial, sValidatedDlCursorCache[1], sValidatedReadCache[1];
static int handledEvents;
static void handleEvents(void) { ++handledEvents; }
static void swapEnd(void) {}
static void finishRender(void) {}
static struct { void (*handle_events)(void); void (*swap_buffers_end)(void); } windowApi = {handleEvents, swapEnd};
static struct { void (*finish_render)(void); } renderApi = {finishRender};
static __attribute__((unused)) typeof(windowApi)* gfx_wapi = &windowApi;
static __attribute__((unused)) typeof(renderApi)* gfx_rapi = &renderApi;
static bool dropped_frame;
static struct { int height; } gfx_current_dimensions = {240};
#define SCR_WIDTH 320
#define SCR_HEIGHT 240
#define BUF_WIDTH 320
static uint16_t depthPixels[320 * 240];
static void* sDepthBuffer = depthPixels;
static void* gfx_scegu_vram_cpu_addr(const void* address) { return (void*)address; }
typedef int SceUID;
typedef unsigned SceSize;
#define PSP_THREAD_ATTR_USER 1
#define PSP_THREAD_ATTR_VFPU 2
#define GU_SYNC_FINISH 0
#define GU_SYNC_WHAT_DONE 0
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned counts[3], semaphores, deletedSemaphores, deletedThreads;
static int failure, createCalls, phase, logicTicks, finishes, presents;
static bool holdGe, holdVblank, releaseOnJoin;
static int (*entry)(SceSize, void*);
static SceUID sceKernelCreateSema(const char* name, int attr, int initial, int max, void* option) {
    (void)name; (void)attr; (void)option;
    assert(initial == 0 && max == 1);
    ++createCalls;
    if (failure == createCalls) return -1;
    return ++semaphores;
}
static int sceKernelDeleteSema(SceUID id) { assert(id > 0); ++deletedSemaphores; return 0; }
static int sceKernelWaitSema(SceUID id, int n, void* timeout) {
    assert(id > 0 && id <= 2 && n == 1 && timeout == NULL);
    pthread_mutex_lock(&mutex);
    if (id == 2 && releaseOnJoin) {
        holdGe = holdVblank = false;
        releaseOnJoin = false;
        pthread_cond_broadcast(&changed);
    }
    while (!counts[id]) pthread_cond_wait(&changed, &mutex);
    --counts[id];
    pthread_mutex_unlock(&mutex);
    return 0;
}
static int sceKernelSignalSema(SceUID id, int n) {
    assert(id > 0 && id <= 2 && n == 1);
    pthread_mutex_lock(&mutex);
    assert(counts[id] == 0);
    ++counts[id];
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
    return 0;
}
static SceUID sceKernelCreateThread(const char* name, int (*run)(SceSize, void*), int priority,
                                   int stack, int attr, void* option) {
    (void)name; (void)option;
    assert(priority < 0x20 && priority > 0x1e && stack >= 0x10000);
    assert(attr == (PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU));
    if (failure == 3) return -1;
    entry = run;
    return 3;
}
static void* runThread(void* unused) { (void)unused; entry(0, NULL); return NULL; }
static int sceKernelStartThread(SceUID id, int size, void* args) {
    assert(id == 3 && size == 0 && args == NULL);
    if (failure == 4) return -1;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, runThread, NULL) == 0);
    assert(pthread_detach(thread) == 0);
    return 0;
}
static int sceKernelDeleteThread(SceUID id) { assert(id == 3); ++deletedThreads; return 0; }

static void* sDrawBuffer = (void*)1;
static void* sDisplayBuffer = (void*)2;
static int sColorBufferCount = 2, sFrameVSync, vsync = 1;
static bool sInterlacedOutput;
static void sceGuFinish(void) { ++finishes; }
static void sceGuSync(int mode, int what) {
    (void)mode; (void)what;
    pthread_mutex_lock(&mutex);
    phase = 1;
    pthread_cond_broadcast(&changed);
    while (holdGe) pthread_cond_wait(&changed, &mutex);
    depthPixels[0] = 0x1234;
    pthread_mutex_unlock(&mutex);
}
static void sceDisplayWaitVblankStart(void) {
    pthread_mutex_lock(&mutex);
    phase = 2;
    pthread_cond_broadcast(&changed);
    while (holdVblank) pthread_cond_wait(&changed, &mutex);
    pthread_mutex_unlock(&mutex);
}
static void* sceGuSwapBuffers(void) { ++presents; return sDisplayBuffer; }
static void gfx_scegu_present_interlaced(void) { ++presents; }
static void gfx_scegu_present_triple(int enabled) { assert(enabled == 0 || enabled == 1); ++presents; }
static int OotPspVideo_IsVSyncEnabled(void) { return vsync; }
'''

tests = r'''
static void waitPhase(int expected) {
    pthread_mutex_lock(&mutex);
    while (phase != expected) pthread_cond_wait(&changed, &mutex);
    pthread_mutex_unlock(&mutex);
}
int main(int argc, char** argv) {
    failure = argc > 1 ? atoi(argv[1]) : 0;
    OotPspVideoThread_Wait(); // No frame, no initialization or kernel wait.
    assert(createCalls == 0);
    if (failure) {
        for (int i = 0; i < 3; ++i) gfx_scegu_end_frame();
        assert(finishes == 3 && presents == 3 && !sFramePending);
        assert(deletedSemaphores == semaphores);
        assert(deletedThreads == (failure == 4));
        assert(createCalls == (failure == 1 ? 1 : 2));
        return 0;
    }

    holdGe = holdVblank = true;
    sNativeMeshes[0].pinned = sNativeMeshes[1].pinned = true;
    gfx_scegu_end_frame();
    waitPhase(1);
    gfx_end_frame();
    assert(sNativeMeshes[0].pinned && sNativeMeshes[1].pinned);
    // Submission returned while the video worker is blocked in GE completion.
    assert(finishes == 1 && presents == 0 && sFramePending);
    ++logicTicks;
    pthread_mutex_lock(&mutex);
    holdGe = false;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
    waitPhase(2);
    assert(presents == 0);
    ++logicTicks; // CPU also advances logic during the worker's vblank wait.
    pthread_mutex_lock(&mutex);
    holdVblank = false;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
    gfx_start_frame(); // Join before releasing pins or touching render storage.
    assert(presents == 1 && logicTicks == 2 && !sFramePending);
    assert(!sNativeMeshes[0].pinned && !sNativeMeshes[1].pinned && handledEvents == 1);
    assert(sDisplayBuffer == (void*)1 && sDrawBuffer == (void*)2);

    // Joining before the next frame protects a single command/vertex arena.
    // Capture VSync before transfer: a later UI toggle applies to the next job.
    holdGe = holdVblank = true;
    phase = 0;
    gfx_scegu_end_frame();
    waitPhase(1);
    vsync = 0;
    pthread_mutex_lock(&mutex);
    holdGe = false;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
    waitPhase(2); // Still uses this submitted frame's VSync value.
    releaseOnJoin = true;
    uint16_t depth = 0;
    assert(gfx_scegu_read_depth(0, 0, &depth) && depth == 0x1234);
    assert(presents == 2 && !sFramePending);

    for (int i = 0; i < 200; ++i) {
        gfx_scegu_end_frame();
        OotPspVideoThread_Wait();
        assert(presents == i + 3 && !sFramePending);
    }
    assert(semaphores == 2 && createCalls == 2 && deletedSemaphores == 0);

    // A second submission must retire its predecessor before replacing the job.
    holdGe = holdVblank = true;
    phase = 0;
    OotPspVideoThread_Submit(gfx_scegu_complete_frame);
    waitPhase(1);
    releaseOnJoin = true;
    OotPspVideoThread_Submit(gfx_scegu_complete_frame);
    OotPspVideoThread_Wait();
    assert(presents == 204);
    puts("Video thread: GE/vblank overlap, ownership fence, VSync snapshot and repeated handoffs passed");
}
'''

code = stubs + re.sub(r"^#include [^\n]+\n", "", worker, flags=re.M)
code += function(backend, "gfx_scegu_complete_frame") + function(backend, "gfx_scegu_end_frame")
code += function(backend, "gfx_scegu_read_depth")
code += function(renderer, "gfx_start_frame") + function(renderer, "gfx_end_frame") + tests
with tempfile.TemporaryDirectory(prefix="oot-video-thread-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(code)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-pthread", str(path / "test.c"), "-o", str(path / "test")], check=True)
    for failure in range(5):
        subprocess.run([str(path / "test"), str(failure)], check=True, timeout=15)
    print("Video thread: synchronous fallback and cleanup at every initialization failure passed")
