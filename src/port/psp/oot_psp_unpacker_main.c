#include <pspkernel.h>
#include <psppower.h>
#include <stdio.h>
#include <string.h>
#include "oot_psp_asset_builder.h"
#include "oot_psp_asset_loader.h"
#include "oot_psp_unpacker_ui.h"

PSP_MODULE_INFO("OOT PSP Unpacker", 0, 1, 0);
PSP_HEAP_SIZE_KB(-1024);

extern void __libcglue_init(int argc, char* argv[]);
extern void __libcglue_deinit(void);

/* A returning module entry is essential: newlib's normal main/exit path
 * self-unloads, which cannot complete while sceKernelStartModule is waiting
 * on this very entry. The launcher owns stop/unload after this returns. */
int module_start(SceSize argumentSize, void* arguments) {
    char* argv[3] = {NULL, NULL, NULL};
    char* cursor = arguments;
    size_t remaining = argumentSize;
    int argc = 0, result;
    while (remaining && argc < 2) {
        char* end = memchr(cursor, '\0', remaining);
        if (!end) return 1;
        argv[argc++] = cursor;
        remaining -= (size_t)(end - cursor) + 1;
        cursor = end + 1;
    }
    if (!argc) return 1;
    __libcglue_init(argc, argv);
    scePowerSetClockFrequency(333, 333, 166);
    result = !OotPsp_SetAssetRoot(argc > 1 ? argv[1] : argv[0]) || !OotPspAssetBuilder_Ensure();
    if (!result) OotPspUnpackerUI_Draw(1000, "Starting game...", false);
    else sceKernelDelayThread(3000000);
    OotPspUnpackerUI_Shutdown();
    fflush(NULL);
    __libcglue_deinit();
    return result;
}

int module_stop(SceSize argumentSize, void* arguments) {
    (void)argumentSize; (void)arguments;
    return 0;
}

/* Required by libc's fatal-error path; normal extraction never calls exit. */
void _exit(int status) {
    (void)status;
    sceKernelExitGame();
    for (;;) sceKernelSleepThread();
}
