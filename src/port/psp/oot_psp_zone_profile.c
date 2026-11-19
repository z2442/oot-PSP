#include "oot_psp_zone_profile.h"

#if defined(OOT_PSP_ZONE_PROFILE) && OOT_PSP_ZONE_PROFILE

#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>

#include "oot_psp_asset_loader.h"

#define OOT_PSP_ZONE_SAMPLE_USEC 200

volatile unsigned char gOotPspZone;

static const char* const sZoneNames[OOT_PSP_ZONE_COUNT] = {
    "other (main thread, unmarked)",
    "wait (GE/vblank/pacing/sync)",
    "game update + actor draw (GameState_Update)",
    "render: display-list dispatch",
    "render: G_VTX vertex load/transform",
    "render: G_MTX matrix",
    "render: triangle setup/cull",
    "render: clipping",
    "render: vertex emit",
    "render: material state prepare",
    "render: texture import/cache/bind",
    "render: texture/tile/palette commands",
    "render: combiner",
    "render: batch flush/GE submit",
    "render: segment address resolve",
    "render: native mesh",
    "render: rectangles/2D",
    "render: frame start/end",
};

static volatile unsigned int sCounts[OOT_PSP_ZONE_COUNT];
static volatile unsigned int sFrames;
static volatile int sCapturing;
static SceUID sTimer = -1;
static int sComboHeld;

static SceUInt OotPspZoneProfile_Tick(SceUID uid, SceKernelSysClock* requested, SceKernelSysClock* actual,
                                      void* common) {
    unsigned char zone = gOotPspZone;

    (void)uid;
    (void)requested;
    (void)actual;
    (void)common;
    if (sCapturing && zone < OOT_PSP_ZONE_COUNT) {
        sCounts[zone]++;
    }
    return OOT_PSP_ZONE_SAMPLE_USEC;
}

static void OotPspZoneProfile_StartTimer(void) {
    SceKernelSysClock clock;

    if (sTimer >= 0) {
        return;
    }
    sTimer = sceKernelCreateVTimer("oot-zone-profile", NULL);
    if (sTimer < 0) {
        return;
    }
    clock.hi = 0;
    clock.low = OOT_PSP_ZONE_SAMPLE_USEC;
    sceKernelSetVTimerHandler(sTimer, &clock, OotPspZoneProfile_Tick, NULL);
    sceKernelStartVTimer(sTimer);
}

void OotPspZoneProfile_FrameRendered(void) {
    if (sCapturing) {
        sFrames++;
    }
}

static void OotPspZoneProfile_Write(void) {
    char path[384];
    char line[160];
    const char* resolved = OotPsp_ResolveRootPath("oot-psp-zones.txt", path, sizeof(path));
    unsigned int counts[OOT_PSP_ZONE_COUNT];
    unsigned int total = 0;
    unsigned int frames = sFrames;
    SceUID fd;
    int i;

    for (i = 0; i < OOT_PSP_ZONE_COUNT; i++) {
        counts[i] = sCounts[i];
        total += counts[i];
    }
    fd = sceIoOpen(resolved, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        printf("oot-psp zone profile: cannot write %s\n", resolved);
        return;
    }
    snprintf(line, sizeof(line), "samples=%u interval_us=%d frames=%u ms_per_frame=%.2f\n", total,
             OOT_PSP_ZONE_SAMPLE_USEC, frames,
             frames ? (double)total * OOT_PSP_ZONE_SAMPLE_USEC / 1000.0 / frames : 0.0);
    sceIoWrite(fd, line, strlen(line));
    for (i = 0; i < OOT_PSP_ZONE_COUNT; i++) {
        snprintf(line, sizeof(line), "%6.2f%%  %7.3f ms/frame  %s\n",
                 total ? 100.0 * counts[i] / total : 0.0,
                 frames ? (double)counts[i] * OOT_PSP_ZONE_SAMPLE_USEC / 1000.0 / frames : 0.0, sZoneNames[i]);
        sceIoWrite(fd, line, strlen(line));
    }
    sceIoClose(fd);
    printf("oot-psp zone profile written to %s (%u samples, %u frames)\n", resolved, total, frames);
}

void OotPspZoneProfile_OnButtons(unsigned int buttons) {
    const unsigned int combo = PSP_CTRL_SELECT | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER;
    const int held = (buttons & combo) == combo;
    int i;

    if (!held || sComboHeld) {
        sComboHeld = held;
        return;
    }
    sComboHeld = 1;

    if (!sCapturing) {
        OotPspZoneProfile_StartTimer();
        for (i = 0; i < OOT_PSP_ZONE_COUNT; i++) {
            sCounts[i] = 0;
        }
        sFrames = 0;
        sCapturing = 1;
        printf("oot-psp zone profile capture started\n");
    } else {
        sCapturing = 0;
        OotPspZoneProfile_Write();
    }
}

#endif
