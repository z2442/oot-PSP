#include <pspfpu.h>
#include <pspkernel.h>
#include <string.h>

#include "attributes.h"
#include "controller.h"
#include "console_logo_state.h"
#include "fault.h"
#include "game.h"
#include "gfx.h"
#include "libc64/malloc.h"
#include "oot_psp_asset_loader.h"
#include "play_state.h"
#include "setup_state.h"
#include "title_setup_state.h"


PSP_MODULE_INFO("OOT PSP Port", 0, 1, 0);
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

int main(int argc, char** argv) {

    pspFpuSetEnable(0);

    GraphicsContext gfxCtx;
    GameState* gameState;
    GameStateFunc nextInit = Setup_Init;
    size_t nextSize = sizeof(SetupState);

    osInitialize();
    osSyncPrintf("oot-psp probe rom md5=%s size=%u header=%02X%02X%02X%02X\n", gOotPspRomMd5, gOotPspRomSize,
                 gOotPspRomHeader[0], gOotPspRomHeader[1], gOotPspRomHeader[2], gOotPspRomHeader[3]);
    OotPsp_AssetInit(((argc > 0) && (argv != NULL)) ? argv[0] : NULL);
    OotPspGame_Init();
    Graph_Init(&gfxCtx);

    while (nextInit != NULL) {
        gameState = SYSTEM_ARENA_MALLOC(nextSize, __FILE__, __LINE__);
        if (gameState == NULL) {
            Fault_AddHungupAndCrash(__FILE__, __LINE__);
        }

        memset(gameState, 0, nextSize);
        osSyncPrintf("oot-psp boot state init=%p size=%u\n", nextInit, (u32)nextSize);
        GameState_Init(gameState, nextInit, &gfxCtx);

        while (GameState_IsRunning(gameState)) {
            Graph_Update(&gfxCtx, gameState);

            if (CHECK_BTN_ALL(gameState->input[0].cur.button, BTN_START | BTN_Z)) {
                gameState->running = false;
                nextInit = NULL;
                break;
            }
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

    Graph_Destroy(&gfxCtx);
    sceKernelExitGame();
    return 0;
}
