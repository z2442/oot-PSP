#ifndef OOT_PSP_LAUNCHER_PATH_H
#define OOT_PSP_LAUNCHER_PATH_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Resolve argv[0] before changing directory. Keep the launch device (including
 * ef0: on PSP Go) rather than assuming a Memory Stick or an install folder. */
static int OotPspLauncher_ResolveExecutable(const char* argument, const char* cwd,
                                           char* output, size_t size) {
    char joined[1024];
    char* colon;
    char* cursor;
    size_t used, deviceLength;
    int written;

    if (argument == NULL || argument[0] == '\0') argument = "EBOOT.PBP";
    if (strchr(argument, ':') != NULL) {
        written = snprintf(joined, sizeof(joined), "%s", argument);
    } else if (cwd != NULL && strchr(cwd, ':') != NULL) {
        if (argument[0] == '/' || argument[0] == '\\') {
            written = snprintf(joined, sizeof(joined), "%.*s%s",
                               (int)(strchr(cwd, ':') - cwd + 1), cwd, argument);
        } else {
            written = snprintf(joined, sizeof(joined), "%s/%s", cwd, argument);
        }
    } else {
        return 0;
    }
    if (written < 0 || (size_t)written >= sizeof(joined)) return 0;
    for (cursor = joined; *cursor; cursor++) {
        if (*cursor == '\\') *cursor = '/';
    }
    colon = strchr(joined, ':');
    if (colon == joined || colon == NULL || colon[1] != '/' ||
        memchr(joined, '/', (size_t)(colon - joined)) != NULL) return 0;
    deviceLength = (size_t)(colon - joined) + 2;
    if (deviceLength >= size) return 0;
    memcpy(output, joined, deviceLength);
    used = deviceLength;
    cursor = colon + 2;
    while (*cursor) {
        char* component;
        size_t length;
        while (*cursor == '/') cursor++;
        component = cursor;
        while (*cursor && *cursor != '/') cursor++;
        length = (size_t)(cursor - component);
        if (!length || (length == 1 && component[0] == '.')) continue;
        if (length == 2 && component[0] == '.' && component[1] == '.') {
            if (used == deviceLength) return 0;
            while (used > deviceLength && output[used - 1] != '/') used--;
            if (used > deviceLength) used--;
            continue;
        }
        if (memchr(component, ':', length) != NULL) return 0;
        if (used + (used > deviceLength) + length >= size) return 0;
        if (used > deviceLength) output[used++] = '/';
        memcpy(output + used, component, length);
        used += length;
    }
    output[used] = '\0';
    return used > deviceLength;
}

#endif
