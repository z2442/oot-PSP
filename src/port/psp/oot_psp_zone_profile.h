#ifndef OOT_PSP_ZONE_PROFILE_H
#define OOT_PSP_ZONE_PROFILE_H

/* Low-overhead sampling profiler for release-optimized PSP builds
 * (make psp-port PSP_ZONE_PROFILE=1).
 *
 * Code marks the main thread's current stage with OOT_PSP_ZONE(id): one load
 * and one store on entry, one store on scope exit (including early returns).
 * A 5 kHz VTimer counts the zone it interrupts. Unlike the PSPSDK gprof
 * sampler, which charges each tick to the most recently entered -pg function,
 * time is attributed to the stage actually running, and the build keeps its
 * normal optimization and code layout.
 *
 * Limits: the VTimer cannot tell which thread it interrupted. Main-thread
 * blocking points are marked WAIT; preemption by audio threads is charged to
 * whatever zone the main thread was in, spreading it proportionally.
 *
 * SELECT+L+R starts a capture (clearing counts); pressing it again writes
 * oot-psp-zones.txt next to EBOOT.PBP. */

enum OotPspZone {
    OOT_PSP_ZONE_OTHER,
    OOT_PSP_ZONE_WAIT,
    OOT_PSP_ZONE_GAME_UPDATE,
    OOT_PSP_ZONE_RENDER_DL,
    OOT_PSP_ZONE_RENDER_VTX,
    OOT_PSP_ZONE_RENDER_MTX,
    OOT_PSP_ZONE_RENDER_TRI,
    OOT_PSP_ZONE_RENDER_CLIP,
    OOT_PSP_ZONE_RENDER_EMIT,
    OOT_PSP_ZONE_RENDER_PREPARE,
    OOT_PSP_ZONE_RENDER_TEX_IMPORT,
    OOT_PSP_ZONE_RENDER_TEX_CMDS,
    OOT_PSP_ZONE_RENDER_COMBINE,
    OOT_PSP_ZONE_RENDER_FLUSH,
    OOT_PSP_ZONE_RENDER_SEG_ADDR,
    OOT_PSP_ZONE_RENDER_NATIVE_MESH,
    OOT_PSP_ZONE_RENDER_RECT,
    OOT_PSP_ZONE_RENDER_FRAME,
    OOT_PSP_ZONE_COUNT
};

#if defined(OOT_PSP_ZONE_PROFILE) && OOT_PSP_ZONE_PROFILE

extern volatile unsigned char gOotPspZone;

static inline __attribute__((always_inline)) unsigned char OotPspZone_Swap(unsigned char zone) {
    unsigned char previous = gOotPspZone;

    gOotPspZone = zone;
    return previous;
}

static inline __attribute__((always_inline)) void OotPspZone_Restore(unsigned char* previous) {
    gOotPspZone = *previous;
}

#define OOT_PSP_ZONE_CONCAT2(a, b) a##b
#define OOT_PSP_ZONE_CONCAT(a, b) OOT_PSP_ZONE_CONCAT2(a, b)
/* Marks the rest of the enclosing scope. */
#define OOT_PSP_ZONE(zone)                                                               \
    unsigned char OOT_PSP_ZONE_CONCAT(sOotPspZonePrev, __LINE__)                         \
        __attribute__((cleanup(OotPspZone_Restore), unused)) = OotPspZone_Swap(zone)

void OotPspZoneProfile_OnButtons(unsigned int buttons);
void OotPspZoneProfile_FrameRendered(void);

#else

#define OOT_PSP_ZONE(zone) ((void)0)
#define OotPspZoneProfile_OnButtons(buttons) ((void)0)
#define OotPspZoneProfile_FrameRendered() ((void)0)

#endif

#endif
