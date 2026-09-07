#include "oot_psp_asset_loader.h"

#include <stdio.h>
#include <string.h>

static char sOotPspAssetRoot[256];

s32 OotPsp_SetAssetRoot(const char* executablePath) {
    const char* slash;
    const char* backslash;
    size_t length;

    sOotPspAssetRoot[0] = '\0';

    if ((executablePath == NULL) || (executablePath[0] == '\0')) {
        printf("oot-psp asset root missing argv0\n");
        return false;
    }

    slash = strrchr(executablePath, '/');
    backslash = strrchr(executablePath, '\\');
    if ((backslash != NULL) && ((slash == NULL) || (backslash > slash))) {
        slash = backslash;
    }

    if (slash == NULL) {
        printf("oot-psp asset root no directory argv0=%s\n", executablePath);
        return false;
    }

    length = (size_t)(slash - executablePath) + 1;
    if (length >= sizeof(sOotPspAssetRoot)) {
        printf("oot-psp asset root is too long\n");
        return false;
    }

    memcpy(sOotPspAssetRoot, executablePath, length);
    sOotPspAssetRoot[length] = '\0';
    printf("oot-psp asset root=%s\n", sOotPspAssetRoot);
    return true;
}

static s32 OotPsp_IsAbsolutePath(const char* path) {
    const char* slash;
    const char* colon;

    if ((path == NULL) || (path[0] == '\0')) {
        return false;
    }

    if ((path[0] == '/') || (path[0] == '\\')) {
        return true;
    }

    colon = strchr(path, ':');
    slash = strpbrk(path, "/\\");
    return (colon != NULL) && ((slash == NULL) || (colon < slash));
}

const char* OotPsp_ResolveRootPath(const char* path, char* buffer, size_t bufferSize) {
    int written;

    if (OotPsp_IsAbsolutePath(path) || (sOotPspAssetRoot[0] == '\0')) {
        return path;
    }

    written = snprintf(buffer, bufferSize, "%s%s", sOotPspAssetRoot, path);
    if ((written < 0) || ((size_t)written >= bufferSize)) {
        printf("oot-psp root path too long root=%s path=%s\n", sOotPspAssetRoot, path);
        return path;
    }

    return buffer;
}
