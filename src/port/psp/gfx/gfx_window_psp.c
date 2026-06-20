#include "gfx_window_psp.h"

#include <pspctrl.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psprtc.h>

#include "oot_psp_home_menu.h"
#include "oot_port_macros.h"

#define OOT_PSP_SCREEN_WIDTH  480
#define OOT_PSP_SCREEN_HEIGHT 272

static bool sQuitRequested;
static unsigned int sLastSwapBeginUsec;

static int oot_psp_exit_callback(UNUSED int arg1, UNUSED int arg2, UNUSED void* common) {
    OotPspHomeMenu_RequestOpen();
    return 0;
}

static int oot_psp_callback_thread(UNUSED SceSize args, UNUSED void* argp) {
    SceUID cbid = sceKernelCreateCallback("OOT PSP Exit Callback", oot_psp_exit_callback, NULL);
    if (cbid >= 0) {
        sceKernelRegisterExitCallback(cbid);
    }
    sceKernelSleepThreadCB();
    return 0;
}

static void gfx_window_psp_init(UNUSED const char* game_name, UNUSED bool start_in_fullscreen) {
    SceUID thid;

    sQuitRequested = false;
    sLastSwapBeginUsec = sceKernelGetSystemTimeLow();

    scePowerSetClockFrequency(333, 333, 166);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    thid = sceKernelCreateThread("OOTPspCallbackThread", oot_psp_callback_thread, 0x20, 0x1000, 0, NULL);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, NULL);
    }
}

static void gfx_window_psp_set_keyboard_callbacks(UNUSED bool (*on_key_down)(int scancode),
                                                  UNUSED bool (*on_key_up)(int scancode),
                                                  UNUSED void (*on_all_keys_up)(void)) {
}

static void gfx_window_psp_set_fullscreen_changed_callback(UNUSED void (*on_fullscreen_changed)(bool is_now_fullscreen)) {
}

static void gfx_window_psp_set_fullscreen(UNUSED bool enable) {
}

static void gfx_window_psp_main_loop(void (*run_one_game_iter)(void)) {
    while (!sQuitRequested) {
        run_one_game_iter();
    }
}

static void gfx_window_psp_get_dimensions(uint32_t* width, uint32_t* height) {
    *width = OOT_PSP_SCREEN_WIDTH;
    *height = OOT_PSP_SCREEN_HEIGHT;
}

static void gfx_window_psp_handle_events(void) {
    sceKernelDelayThread(100);
}

static bool gfx_window_psp_start_frame(void) {
    return !sQuitRequested;
}

static void gfx_window_psp_swap_buffers_begin(void) {
    sLastSwapBeginUsec = sceKernelGetSystemTimeLow();
}

static void gfx_window_psp_swap_buffers_end(void) {
    /*
     * Presentation is handled by the SCE GU backend's end_frame hook.
     */
}

static double gfx_window_psp_get_time(void) {
    u64 ticks = 0;

    sceRtcGetCurrentTick(&ticks);
    return (double)ticks;
}

struct GfxWindowManagerAPI gfx_psp_window_api = {
    gfx_window_psp_init,
    gfx_window_psp_set_keyboard_callbacks,
    gfx_window_psp_set_fullscreen_changed_callback,
    gfx_window_psp_set_fullscreen,
    gfx_window_psp_main_loop,
    gfx_window_psp_get_dimensions,
    gfx_window_psp_handle_events,
    gfx_window_psp_start_frame,
    gfx_window_psp_swap_buffers_begin,
    gfx_window_psp_swap_buffers_end,
    gfx_window_psp_get_time,
};

bool OotPspWindow_ShouldQuit(void) {
    return sQuitRequested;
}
