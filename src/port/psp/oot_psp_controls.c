#include "oot_psp_controls.h"

#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "controller.h"
#include "oot_psp_asset_loader.h"

#define OOT_PSP_CONTROLS_INI_BUFFER_SIZE 4096

typedef struct OotPspButtonBinding {
    const char* iniName;
    const char* displayName;
    u32 pspMask;
    u16 defaultN64Mask;
    u16 n64Mask;
} OotPspButtonBinding;

typedef struct OotPspN64Action {
    const char* iniName;
    const char* displayName;
    u16 n64Mask;
} OotPspN64Action;

static OotPspButtonBinding sButtonBindings[] = {
    { "cross", "Cross", PSP_CTRL_CROSS, BTN_A, BTN_A },
    { "circle", "Circle", PSP_CTRL_CIRCLE, BTN_B, BTN_B },
    { "triangle", "Triangle", PSP_CTRL_TRIANGLE, BTN_CUP, BTN_CUP },
    { "square", "Square", PSP_CTRL_SQUARE, BTN_CLEFT, BTN_CLEFT },
    { "ltrigger", "L Trigger", PSP_CTRL_LTRIGGER, BTN_Z, BTN_Z },
    { "rtrigger", "R Trigger", PSP_CTRL_RTRIGGER, BTN_R, BTN_R },
    { "start", "Start", PSP_CTRL_START, BTN_START, BTN_START },
    { "select", "Select", PSP_CTRL_SELECT, BTN_L, BTN_L },
    { "up", "D-Pad Up", PSP_CTRL_UP, BTN_CUP, BTN_CUP },
    { "down", "D-Pad Down", PSP_CTRL_DOWN, BTN_CDOWN, BTN_CDOWN },
    { "left", "D-Pad Left", PSP_CTRL_LEFT, BTN_CLEFT, BTN_CLEFT },
    { "right", "D-Pad Right", PSP_CTRL_RIGHT, BTN_CRIGHT, BTN_CRIGHT },
};

static const OotPspN64Action sN64Actions[] = {
    { "none", "None", 0 },
    { "A", "A", BTN_A },
    { "B", "B", BTN_B },
    { "Z", "Z", BTN_Z },
    { "Start", "Start", BTN_START },
    { "D-Up", "D-Up", BTN_DUP },
    { "D-Down", "D-Down", BTN_DDOWN },
    { "D-Left", "D-Left", BTN_DLEFT },
    { "D-Right", "D-Right", BTN_DRIGHT },
    { "L", "L", BTN_L },
    { "R", "R", BTN_R },
    { "C-Up", "C-Up", BTN_CUP },
    { "C-Down", "C-Down", BTN_CDOWN },
    { "C-Left", "C-Left", BTN_CLEFT },
    { "C-Right", "C-Right", BTN_CRIGHT },
};

static int sDeadzone;
static int sInitialized;

static int OotPspControls_StrIcmp(const char* a, const char* b) {
    unsigned char ca;
    unsigned char cb;

    while ((*a != '\0') && (*b != '\0')) {
        ca = (unsigned char)*a;
        cb = (unsigned char)*b;

        if ((ca >= 'A') && (ca <= 'Z')) {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if ((cb >= 'A') && (cb <= 'Z')) {
            cb = (unsigned char)(cb - 'A' + 'a');
        }

        if (ca != cb) {
            return (int)ca - (int)cb;
        }

        a++;
        b++;
    }

    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static char* OotPspControls_Trim(char* text) {
    char* end;

    while ((*text == ' ') || (*text == '\t') || (*text == '\r') || (*text == '\n')) {
        text++;
    }

    end = text + strlen(text);
    while ((end > text) &&
           ((end[-1] == ' ') || (end[-1] == '\t') || (end[-1] == '\r') || (end[-1] == '\n'))) {
        end--;
    }
    *end = '\0';

    return text;
}

static int OotPspControls_ClampDeadzone(int deadzone) {
    if (deadzone < OOT_PSP_CONTROLS_DEADZONE_MIN) {
        return OOT_PSP_CONTROLS_DEADZONE_MIN;
    }
    if (deadzone > OOT_PSP_CONTROLS_DEADZONE_MAX) {
        return OOT_PSP_CONTROLS_DEADZONE_MAX;
    }
    return deadzone;
}

static const char* OotPspControls_Path(char* buffer, size_t bufferSize) {
    return OotPsp_ResolveRootPath(OOT_PSP_CONTROLS_INI_PATH, buffer, bufferSize);
}

static int OotPspControls_FindBinding(const char* name) {
    int i;

    for (i = 0; i < (int)(sizeof(sButtonBindings) / sizeof(sButtonBindings[0])); i++) {
        if (OotPspControls_StrIcmp(name, sButtonBindings[i].iniName) == 0) {
            return i;
        }
    }

    return -1;
}

static const OotPspN64Action* OotPspControls_FindActionByName(const char* name) {
    int i;

    for (i = 0; i < (int)(sizeof(sN64Actions) / sizeof(sN64Actions[0])); i++) {
        if ((OotPspControls_StrIcmp(name, sN64Actions[i].iniName) == 0) ||
            (OotPspControls_StrIcmp(name, sN64Actions[i].displayName) == 0)) {
            return &sN64Actions[i];
        }
    }

    return NULL;
}

static int OotPspControls_FindActionIndexByMask(u16 n64Mask) {
    int i;

    for (i = 0; i < (int)(sizeof(sN64Actions) / sizeof(sN64Actions[0])); i++) {
        if (sN64Actions[i].n64Mask == n64Mask) {
            return i;
        }
    }

    return 0;
}

static u16 OotPspControls_ParseActionList(char* value) {
    u16 n64Mask = 0;
    char* token;

    for (token = strtok(value, ",|+"); token != NULL; token = strtok(NULL, ",|+")) {
        const OotPspN64Action* action = OotPspControls_FindActionByName(OotPspControls_Trim(token));

        if (action != NULL) {
            n64Mask |= action->n64Mask;
        }
    }

    return n64Mask;
}

void OotPspControls_InitDefaults(void) {
    int i;

    for (i = 0; i < (int)(sizeof(sButtonBindings) / sizeof(sButtonBindings[0])); i++) {
        sButtonBindings[i].n64Mask = sButtonBindings[i].defaultN64Mask;
    }

    sDeadzone = 0;
    sInitialized = true;
}

void OotPspControls_ResetDefaults(void) {
    OotPspControls_InitDefaults();
}

static void OotPspControls_EnsureInitialized(void) {
    if (!sInitialized) {
        OotPspControls_InitDefaults();
    }
}

s32 OotPspControls_Load(void) {
    char pathBuffer[384];
    const char* path;
    char iniBuffer[OOT_PSP_CONTROLS_INI_BUFFER_SIZE];
    SceUID fd;
    int readSize;
    char* line;
    char* nextLine;
    int section = 0;

    OotPspControls_InitDefaults();

    path = OotPspControls_Path(pathBuffer, sizeof(pathBuffer));
    fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) {
        return OotPspControls_Save();
    }

    readSize = sceIoRead(fd, iniBuffer, sizeof(iniBuffer) - 1);
    sceIoClose(fd);
    if (readSize < 0) {
        return readSize;
    }

    iniBuffer[readSize] = '\0';

    for (line = iniBuffer; line != NULL; line = nextLine) {
        char* comment;
        char* equals;
        char* key;
        char* value;

        nextLine = strchr(line, '\n');
        if (nextLine != NULL) {
            *nextLine = '\0';
            nextLine++;
        }

        comment = strpbrk(line, "#;");
        if (comment != NULL) {
            *comment = '\0';
        }

        line = OotPspControls_Trim(line);
        if (line[0] == '\0') {
            continue;
        }

        if (line[0] == '[') {
            char* sectionEnd = strchr(line, ']');

            if (sectionEnd != NULL) {
                *sectionEnd = '\0';
                if (OotPspControls_StrIcmp(line + 1, "buttons") == 0) {
                    section = 1;
                } else if (OotPspControls_StrIcmp(line + 1, "analog") == 0) {
                    section = 2;
                } else {
                    section = 0;
                }
            }
            continue;
        }

        equals = strchr(line, '=');
        if (equals == NULL) {
            continue;
        }

        *equals = '\0';
        key = OotPspControls_Trim(line);
        value = OotPspControls_Trim(equals + 1);

        if (section == 1) {
            int bindingIndex = OotPspControls_FindBinding(key);

            if (bindingIndex >= 0) {
                sButtonBindings[bindingIndex].n64Mask = OotPspControls_ParseActionList(value);
            }
        } else if ((section == 2) && (OotPspControls_StrIcmp(key, "deadzone") == 0)) {
            OotPspControls_SetDeadzone(atoi(value));
        }
    }

    return 0;
}

s32 OotPspControls_Save(void) {
    char pathBuffer[384];
    const char* path;
    char iniBuffer[OOT_PSP_CONTROLS_INI_BUFFER_SIZE];
    char valueBuffer[96];
    int written;
    int offset = 0;
    int i;
    SceUID fd;

    OotPspControls_EnsureInitialized();

#define OOT_PSP_CONTROLS_APPEND(...)                                                        \
    do {                                                                                     \
        written = snprintf(iniBuffer + offset, sizeof(iniBuffer) - (size_t)offset, __VA_ARGS__); \
        if ((written < 0) || ((size_t)written >= sizeof(iniBuffer) - (size_t)offset)) {      \
            return -1;                                                                       \
        }                                                                                    \
        offset += written;                                                                   \
    } while (0)

    OOT_PSP_CONTROLS_APPEND("[buttons]\n");
    for (i = 0; i < (int)(sizeof(sButtonBindings) / sizeof(sButtonBindings[0])); i++) {
        OotPspControls_GetBindingValueText(i, valueBuffer, sizeof(valueBuffer));
        OOT_PSP_CONTROLS_APPEND("%s = %s\n", sButtonBindings[i].iniName, valueBuffer);
    }

    OOT_PSP_CONTROLS_APPEND("\n[analog]\n");
    OOT_PSP_CONTROLS_APPEND("deadzone = %d\n", sDeadzone);

#undef OOT_PSP_CONTROLS_APPEND

    path = OotPspControls_Path(pathBuffer, sizeof(pathBuffer));
    fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        return fd;
    }

    written = sceIoWrite(fd, iniBuffer, offset);
    sceIoClose(fd);
    return (written == offset) ? 0 : -1;
}

u16 OotPspControls_MapButtons(u32 pspButtons) {
    u16 n64Buttons = 0;
    int i;

    OotPspControls_EnsureInitialized();

    for (i = 0; i < (int)(sizeof(sButtonBindings) / sizeof(sButtonBindings[0])); i++) {
        if (pspButtons & sButtonBindings[i].pspMask) {
            n64Buttons |= sButtonBindings[i].n64Mask;
        }
    }

    return n64Buttons;
}

s8 OotPspControls_MapStick(u8 raw) {
    s32 centered = (s32)raw - 128;

    OotPspControls_EnsureInitialized();

    if ((centered >= -sDeadzone) && (centered <= sDeadzone)) {
        centered = 0;
    }

    if (centered < -80) {
        centered = -80;
    }
    if (centered > 80) {
        centered = 80;
    }

    return centered;
}

int OotPspControls_GetBindingCount(void) {
    return (int)(sizeof(sButtonBindings) / sizeof(sButtonBindings[0]));
}

const char* OotPspControls_GetBindingName(int index) {
    if ((index < 0) || (index >= OotPspControls_GetBindingCount())) {
        return "";
    }

    return sButtonBindings[index].displayName;
}

void OotPspControls_GetBindingValueText(int index, char* buffer, size_t bufferSize) {
    u16 mask;
    size_t used = 0;
    int i;

    if (bufferSize == 0) {
        return;
    }

    buffer[0] = '\0';
    if ((index < 0) || (index >= OotPspControls_GetBindingCount())) {
        return;
    }

    mask = sButtonBindings[index].n64Mask;
    if (mask == 0) {
        snprintf(buffer, bufferSize, "None");
        return;
    }

    for (i = 1; i < (int)(sizeof(sN64Actions) / sizeof(sN64Actions[0])); i++) {
        if (mask & sN64Actions[i].n64Mask) {
            int written = snprintf(buffer + used, bufferSize - used, "%s%s", (used > 0) ? "+" : "",
                                   sN64Actions[i].displayName);

            if ((written < 0) || ((size_t)written >= bufferSize - used)) {
                buffer[bufferSize - 1] = '\0';
                return;
            }
            used += (size_t)written;
        }
    }
}

void OotPspControls_CycleBinding(int index, int direction) {
    int actionIndex;
    int actionCount = (int)(sizeof(sN64Actions) / sizeof(sN64Actions[0]));

    if ((index < 0) || (index >= OotPspControls_GetBindingCount())) {
        return;
    }

    actionIndex = OotPspControls_FindActionIndexByMask(sButtonBindings[index].n64Mask);
    actionIndex += (direction >= 0) ? 1 : -1;

    if (actionIndex < 0) {
        actionIndex = actionCount - 1;
    } else if (actionIndex >= actionCount) {
        actionIndex = 0;
    }

    sButtonBindings[index].n64Mask = sN64Actions[actionIndex].n64Mask;
}

int OotPspControls_GetDeadzone(void) {
    OotPspControls_EnsureInitialized();
    return sDeadzone;
}

void OotPspControls_SetDeadzone(int deadzone) {
    sDeadzone = OotPspControls_ClampDeadzone(deadzone);
}

void OotPspControls_AdjustDeadzone(int delta) {
    OotPspControls_SetDeadzone(sDeadzone + delta);
}
