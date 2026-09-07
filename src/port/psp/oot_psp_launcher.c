#include <pspdebug.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspmodulemgr.h>
#include <pspsdk.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>

#include "oot_psp_asset_identity.h"

#define OOT_PSP_LAUNCHER_PATH_SIZE 512

PSP_MODULE_INFO("OOT PSP Launcher", 0, 1, 0);
PSP_MAIN_THREAD_PRIORITY(0x20);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_MAIN_THREAD_STACK_SIZE_KB(64);
PSP_HEAP_SIZE_KB(256);

static u32 OotPspLauncher_ReadLe32(const u8* data) {
    return (u32)data[0] | ((u32)data[1] << 8) | ((u32)data[2] << 16) | ((u32)data[3] << 24);
}

static s32 OotPspLauncher_GetRoot(const char* executablePath, char* root, size_t rootSize) {
    const char* slash;
    const char* backslash;
    size_t length;

    if ((executablePath == NULL) || (rootSize == 0)) {
        return false;
    }
    slash = strrchr(executablePath, '/');
    backslash = strrchr(executablePath, '\\');
    if ((backslash != NULL) && ((slash == NULL) || (backslash > slash))) {
        slash = backslash;
    }
    if (slash == NULL) {
        return false;
    }
    length = (size_t)(slash - executablePath) + 1;
    if (length >= rootSize) {
        return false;
    }
    memcpy(root, executablePath, length);
    root[length] = '\0';
    return true;
}

static s32 OotPspLauncher_Path(char* output, size_t outputSize, const char* root, const char* relative) {
    int written = snprintf(output, outputSize, "%s%s", root, relative);

    return (written >= 0) && ((size_t)written < outputSize);
}

static s32 OotPspLauncher_ReadIdentity(const char* root, char profile[OOT_PSP_ASSET_ID_PROFILE_SIZE + 1]) {
    char identityPath[OOT_PSP_LAUNCHER_PATH_SIZE];
    char packedPath[OOT_PSP_LAUNCHER_PATH_SIZE];
    u8 data[OOT_PSP_ASSET_ID_SIZE];
    SceUID fd;
    s32 identitySize;
    s32 readSize;
    SceOff packedSize;
    size_t i;

    if (!OotPspLauncher_Path(identityPath, sizeof(identityPath), root, OOT_PSP_PACKED_ASSET_ID_PATH) ||
        !OotPspLauncher_Path(packedPath, sizeof(packedPath), root, OOT_PSP_PACKED_ASSET_PATH)) {
        return false;
    }
    fd = sceIoOpen(identityPath, PSP_O_RDONLY, 0);
    if (fd < 0) {
        return false;
    }
    readSize = sceIoRead(fd, data, sizeof(data));
    identitySize = sceIoLseek32(fd, 0, PSP_SEEK_END);
    if ((readSize != (s32)sizeof(data)) || (identitySize != (s32)sizeof(data))) {
        sceIoClose(fd);
        return false;
    }
    sceIoClose(fd);
    if ((memcmp(data, OOT_PSP_ASSET_ID_MAGIC, 4) != 0) ||
        (OotPspLauncher_ReadLe32(data + 4) != OOT_PSP_ASSET_ID_VERSION)) {
        return false;
    }
    memcpy(profile, data + OOT_PSP_ASSET_ID_PROFILE_OFFSET, OOT_PSP_ASSET_ID_PROFILE_SIZE);
    profile[OOT_PSP_ASSET_ID_PROFILE_SIZE] = '\0';
    for (i = 0; (i < OOT_PSP_ASSET_ID_PROFILE_SIZE) && (profile[i] != '\0'); i++) {
        char value = profile[i];

        if (!(((value >= 'a') && (value <= 'z')) || ((value >= '0') && (value <= '9')) ||
              (value == '-') || (value == '.'))) {
            return false;
        }
    }
    if ((i == 0) || (i == OOT_PSP_ASSET_ID_PROFILE_SIZE)) {
        return false;
    }
    fd = sceIoOpen(packedPath, PSP_O_RDONLY, 0);
    if (fd < 0) {
        return false;
    }
    packedSize = sceIoLseek32(fd, 0, PSP_SEEK_END);
    sceIoClose(fd);
    return (packedSize >= 0) && ((u32)packedSize == OotPspLauncher_ReadLe32(data + OOT_PSP_ASSET_ID_PACKED_SIZE));
}

static s32 OotPspLauncher_Start(const char* modulePath, const char* executablePath) {
    char arguments[OOT_PSP_LAUNCHER_PATH_SIZE * 2];
    size_t moduleLength = strlen(modulePath) + 1;
    size_t executableLength = strlen(executablePath) + 1;
    SceKernelLMOption loadOptions;
    SceKernelSMOption startOptions;
    SceUID module;
    s32 result;
    int status = 0;

    if (moduleLength + executableLength > sizeof(arguments)) {
        return -1;
    }
    memcpy(arguments, modulePath, moduleLength);
    memcpy(arguments + moduleLength, executablePath, executableLength);
    memset(&loadOptions, 0, sizeof(loadOptions));
    loadOptions.size = sizeof(loadOptions);
    loadOptions.mpidtext = PSP_MEMORY_PARTITION_USER;
    loadOptions.mpiddata = PSP_MEMORY_PARTITION_USER;
    module = sceKernelLoadModule(modulePath, 0, &loadOptions);
    if (module < 0) {
        return module;
    }
    memset(&startOptions, 0, sizeof(startOptions));
    startOptions.size = sizeof(startOptions);
    startOptions.mpidstack = PSP_MEMORY_PARTITION_USER;
    /* The unpacker runs synchronously in module_start. Its newlib main-thread
     * stack declaration is bypassed, so allocate the conversion stack here. */
    startOptions.stacksize = 256 * 1024;
    startOptions.priority = 0x20;
    startOptions.attribute = PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU;
    result = sceKernelStartModule(module, moduleLength + executableLength, arguments, &status, &startOptions);
    if (result < 0) {
        sceKernelUnloadModule(module);
        return result;
    }
    if (strstr(modulePath, "unpacker.prx") != NULL) {
        int stopStatus = 0;
        result = sceKernelStopModule(module, 0, NULL, &stopStatus, NULL);
        if (result < 0) return result;
        result = sceKernelUnloadModule(module);
        if (result < 0) return result;
        if (status != 0) return -1;
    }
    return 0;
}

static s32 OotPspLauncher_StartRelative(char* modulePath, size_t modulePathSize, const char* root,
                                        const char* relative, const char* executablePath) {
    char relativePath[OOT_PSP_LAUNCHER_PATH_SIZE];
    if (strlen(relative) >= sizeof(relativePath)) return -1;
    strcpy(relativePath, relative); /* relative may alias modulePath */
    if (!OotPspLauncher_Path(modulePath, modulePathSize, root, relativePath)) return -1;
    return OotPspLauncher_Start(modulePath, executablePath);
}

static void OotPspLauncher_Fail(const char* message, const char* path, s32 error) {
    pspDebugScreenSetXY(0, 0);
    pspDebugScreenPrintf("OOT PSP Launcher\n\n%s\n", message);
    if (path != NULL) {
        pspDebugScreenPrintf("\nPath: %s\n", path);
    }
    pspDebugScreenPrintf("\nError: 0x%08X\n", (unsigned int)error);
    sceKernelDelayThread(5000000);
    sceKernelExitGame();
}

int main(int argc, char** argv) {
    const char* executablePath = ((argc > 0) && (argv != NULL)) ? argv[0] : NULL;
    char root[OOT_PSP_LAUNCHER_PATH_SIZE];
    char modulePath[OOT_PSP_LAUNCHER_PATH_SIZE];
    char profile[OOT_PSP_ASSET_ID_PROFILE_SIZE + 1];
    int written;
    s32 result;

    pspDebugScreenInit();
    if (!OotPspLauncher_GetRoot(executablePath, root, sizeof(root))) {
        OotPspLauncher_Fail("Could not locate the installation directory.", executablePath, -1);
        return 1;
    }
    (void)sceIoChdir(root);
    if (!OotPspLauncher_ReadIdentity(root, profile)) {
        pspDebugScreenPrintf("Preparing Ocarina of Time assets...\n");
        result = OotPspLauncher_StartRelative(modulePath, sizeof(modulePath), root, "Modules/unpacker.prx",
                                              executablePath);
        if (result < 0) {
            OotPspLauncher_Fail("Could not start the asset unpacker.", modulePath, result);
            return 1;
        }
        if (!OotPspLauncher_ReadIdentity(root, profile)) {
            OotPspLauncher_Fail("Asset extraction did not produce a valid identity.", NULL, -1);
            return 1;
        }
    }
    written = snprintf(modulePath, sizeof(modulePath), "Modules/%s.prx", profile);
    if ((written < 0) || ((size_t)written >= sizeof(modulePath))) {
        OotPspLauncher_Fail("The selected game-module path is too long.", NULL, -1);
        return 1;
    }
    pspDebugScreenPrintf("Starting %s...\n", profile);
    result = OotPspLauncher_StartRelative(modulePath, sizeof(modulePath), root, modulePath, executablePath);
    if (result < 0) {
        OotPspLauncher_Fail("Could not start the selected game module.", modulePath, result);
        return 1;
    }

    /* The game module owns process shutdown. Keep this small launcher thread
     * dormant until the game's normal sceKernelExitGame call ends the title. */
    sceKernelSleepThread();
    return 0;
}
