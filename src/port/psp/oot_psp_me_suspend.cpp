// Follow the simple reset variant from mcidclan's me-core sleep-wake sample,
// but leave reset asserted on wake. The game owns the single restart after
// the firmware hook, asset restoration and channel setup have all finished.
#include <me-core-mapper/me-core-custom.h>

// A complete cache line also isolates this uncached counter from cached
// objects that the linker places after this section (including heap state).
static volatile u32 sWakeGeneration[16] __attribute__((aligned(64), section(".uncached")));

static volatile u32* WakeGeneration(void) {
    return (volatile u32*)(UNCACHED_USER_MASK | (uintptr_t)&sWakeGeneration);
}

extern "C" __attribute__((noinline, aligned(4))) void meLibOnSleep(void) {
    HW_SYS_RESET_ENABLE = SC_HW_RESET;
    meLibSync();
}

extern "C" __attribute__((noinline, aligned(4))) void meLibOnWake(void) {
    HW_SYS_RESET_ENABLE = SC_HW_RESET;
    meLibSync();
    // Publish only after the final firmware-hook reset write. A late hook
    // cannot reset a worker that Allegrex has already released for playback.
    (*WakeGeneration())++;
    meLibSync();
}

extern "C" u32 OotPspMeSuspend_WakeGeneration(void) {
    return *WakeGeneration();
}

extern "C" int OotPspMeSuspend_Init(void) {
    return meLibDefaultInit();
}
