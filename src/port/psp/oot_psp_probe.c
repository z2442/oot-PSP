#include <pspfpu.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>

#include "attributes.h"
#include "console_logo_state.h"
#include "fault.h"
#include "game.h"
#include "gfx.h"
#include "libc64/malloc.h"
#include "oot_psp_asset_loader.h"
#include "oot_psp_audio_backend.h"
#include "oot_psp_controls.h"
#include "oot_psp_home_menu.h"
#include "oot_psp_performance.h"
#include "oot_psp_runtime_patch.h"
#include "play_state.h"
#include "setup_state.h"
#include "title_setup_state.h"

#ifdef OOT_PSP_GPROF
#include <pspprof.h>
#endif

PSP_MODULE_INFO("OOT PSP Port", 0, 1, 0);
#define OOT_PSP_MAIN_THREAD_PRIORITY 0x20
PSP_MAIN_THREAD_PRIORITY(OOT_PSP_MAIN_THREAD_PRIORITY);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(1024);
PSP_HEAP_SIZE_KB(-1024);

extern const char gOotPspRomMd5[];
extern const u32 gOotPspRomSize;
extern const u8 gOotPspRomHeader[16];
void OotPspGame_Init(void);
void Graph_Init(GraphicsContext* gfxCtx);
void Graph_Destroy(GraphicsContext* gfxCtx);
void Graph_Update(GraphicsContext* gfxCtx, GameState* gameState);

static size_t OotPspStateSize(GameStateFunc init) {
    if (init == Setup_Init) {
        return sizeof(SetupState);
    }
    if (init == ConsoleLogo_Init) {
        return sizeof(ConsoleLogoState);
    }
    if (init == TitleSetup_Init) {
        return sizeof(TitleSetupState);
    }
    if (init == Play_Init) {
        return sizeof(PlayState);
    }
    return sizeof(GameState);
}

#ifdef OOT_PSP_GPROF
static int sOotPspGprofRunning;
static int sOotPspGprofDumped;
static char sOotPspGprofOutputPath[384];

__attribute__((__no_instrument_function__, __no_profile_instrument_function__))
static void OotPspProfiler_UpdateOutputPath(void) {
    strcpy(sOotPspGprofOutputPath, "gmon.out");
}

__attribute__((__no_instrument_function__, __no_profile_instrument_function__))
static void OotPspProfiler_Start(void) {
    OotPspProfiler_UpdateOutputPath();
    gprof_stop(NULL, 0);
    gprof_start();
    sOotPspGprofRunning = true;
    sOotPspGprofDumped = false;
}

__attribute__((__no_instrument_function__, __no_profile_instrument_function__))
static void OotPspProfiler_Stop(int shouldDump) {
    if (!sOotPspGprofRunning || sOotPspGprofDumped) {
        return;
    }

    gprof_stop(shouldDump ? sOotPspGprofOutputPath : NULL, shouldDump != 0);
    sOotPspGprofRunning = false;
    sOotPspGprofDumped = true;
}
#else
static void OotPspProfiler_Start(void) {
}

static void OotPspProfiler_Stop(UNUSED int shouldDump) {
}
#endif

static int OotPspExitCallback(UNUSED int arg1, UNUSED int arg2, UNUSED void* common) {
    OotPspHomeMenu_RequestOpen();
    return 0;
}

static int OotPspCallbackThread(UNUSED SceSize args, UNUSED void* argp) {
    int callbackId = sceKernelCreateCallback("OOT PSP Exit Callback", OotPspExitCallback, NULL);

    if (callbackId >= 0) {
        sceKernelRegisterExitCallback(callbackId);
        sceKernelSleepThreadCB();
    }

    return 0;
}

static void OotPspSetupCallbacks(void) {
    int threadId = sceKernelCreateThread("OOT PSP Callback Thread", OotPspCallbackThread, 0x11, 0x1000, 0, NULL);

    if (threadId >= 0) {
        sceKernelStartThread(threadId, 0, NULL);
    }
}

int main(int argc, char** argv) {

    pspFpuSetEnable(0);
    (void)OotPspAudioBackend_BootMe();
    OotPspHomeMenu_Init();
    OotPspSetupCallbacks();

    GraphicsContext gfxCtx;
    GameState* gameState;
    GameStateFunc nextInit = Setup_Init;
    size_t nextSize = sizeof(SetupState);

    osInitialize();
    osSyncPrintf("oot-psp probe rom md5=%s size=%u header=%02X%02X%02X%02X\n", gOotPspRomMd5, gOotPspRomSize,
                 gOotPspRomHeader[0], gOotPspRomHeader[1], gOotPspRomHeader[2], gOotPspRomHeader[3]);
    if (!OotPsp_AssetInit(((argc > 0) && (argv != NULL)) ? argv[0] : NULL)) {
        osSyncPrintf("oot-psp asset initialization failed\n");
        sceKernelExitGame();
        return 1;
    }
    if (!OotPspRuntimePatch_Apply()) {
        osSyncPrintf("oot-psp runtime asset patching failed\n");
        sceKernelExitGame();
        return 1;
    }
    OotPspControls_Load();
    OotPspGame_Init();
    Graph_Init(&gfxCtx);
    OotPspProfiler_Start();

    while (nextInit != NULL) {
        gameState = SYSTEM_ARENA_MALLOC(nextSize, __FILE__, __LINE__);
        if (gameState == NULL) {
            Fault_AddHungupAndCrash(__FILE__, __LINE__);
        }

        memset(gameState, 0, nextSize);
        osSyncPrintf("oot-psp boot state init=%p size=%u\n", nextInit, (u32)nextSize);
        GameState_Init(gameState, nextInit, &gfxCtx);

        while (GameState_IsRunning(gameState)) {
            OotPspHomeMenu_PollHomeButton();

            if (OotPspHomeMenu_IsOpen()) {
#if defined(OOTDEBUG)
                OotPspPerformance_Flush();
#endif
                if (OotPspHomeMenu_RunFrame() == OOT_PSP_HOME_MENU_RESULT_EXIT_GAME) {
                    gameState->running = false;
                    nextInit = NULL;
                    break;
                }
                continue;
            }

            Graph_Update(&gfxCtx, gameState);
        }

        if (nextInit == NULL) {
            GameState_Destroy(gameState);
            SYSTEM_ARENA_FREE(gameState, __FILE__, __LINE__);
            break;
        }

        nextInit = GameState_GetInit(gameState);
        nextSize = gameState->size != 0 ? gameState->size : OotPspStateSize(nextInit);
        osSyncPrintf("oot-psp boot state done frames=%u next=%p size=%u\n", gameState->frames, nextInit, (u32)nextSize);
        GameState_Destroy(gameState);
        SYSTEM_ARENA_FREE(gameState, __FILE__, __LINE__);

    }

    OotPspProfiler_Stop(true);
#if defined(OOTDEBUG)
    OotPspPerformance_Flush();
#endif
    Graph_Destroy(&gfxCtx);
    sceKernelExitGame();
    return 0;
}
