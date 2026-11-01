#include "oot_psp_video.h"

#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>

#include "oot_psp_dve.h"

#define OOT_PSP_VIDEO_INI_BUFFER_SIZE 512
#define OOT_PSP_VIDEO_PATH_CAPACITY 384

static OotPspVideoOutput sOotPspVideoOutput;
static OotPspVideoResolution sOotPspVideoResolution;
static OotPspVideoAspect sOotPspVideoAspect;
static int sOotPspVideoVSync = 1;
static OotPspVideoOutput sOotPspVideoActiveOutput;
static OotPspVideoResolution sOotPspVideoActiveResolution;
static OotPspVideoAspect sOotPspVideoActiveAspect;
static char sOotPspVideoPath[OOT_PSP_VIDEO_PATH_CAPACITY];

static int OotPspVideo_StrIcmp(const char* a, const char* b) {
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

static char* OotPspVideo_Trim(char* text) {
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

static void OotPspVideo_BuildPath(const char* executablePath) {
    const char* slash;
    const char* backslash;
    size_t rootLength;
    size_t suffixLength = sizeof(OOT_PSP_VIDEO_INI_PATH);

    sOotPspVideoPath[0] = '\0';
    if ((executablePath == NULL) || (executablePath[0] == '\0')) {
        return;
    }

    slash = strrchr(executablePath, '/');
    backslash = strrchr(executablePath, '\\');
    if ((backslash != NULL) && ((slash == NULL) || (backslash > slash))) {
        slash = backslash;
    }

    rootLength = (slash != NULL) ? (size_t)(slash - executablePath) + 1 : 0;
    if ((rootLength + suffixLength) > sizeof(sOotPspVideoPath)) {
        return;
    }

    if (rootLength != 0) {
        memcpy(sOotPspVideoPath, executablePath, rootLength);
    }
    memcpy(sOotPspVideoPath + rootLength, OOT_PSP_VIDEO_INI_PATH, suffixLength);
}

void OotPspVideo_ResetDefaults(void) {
    sOotPspVideoOutput = OOT_PSP_VIDEO_OUTPUT_LCD;
    sOotPspVideoResolution = OOT_PSP_VIDEO_RESOLUTION_480P;
    sOotPspVideoAspect = OOT_PSP_VIDEO_ASPECT_16_9;
    sOotPspVideoVSync = 1;
}

void OotPspVideo_ForceLcd(void) {
    sOotPspVideoOutput = OOT_PSP_VIDEO_OUTPUT_LCD;
}

void OotPspVideo_CommitMode(void) {
    sOotPspVideoActiveOutput = sOotPspVideoOutput;
    sOotPspVideoActiveResolution = sOotPspVideoResolution;
    sOotPspVideoActiveAspect = sOotPspVideoAspect;
}

void OotPspVideo_Init(const char* executablePath) {
    char buffer[OOT_PSP_VIDEO_INI_BUFFER_SIZE];
    SceUID fd;
    int readSize;
    char* line;
    char* nextLine;

    OotPspVideo_ResetDefaults();
    OotPspVideo_CommitMode();
    OotPspVideo_BuildPath(executablePath);
    if (sOotPspVideoPath[0] == '\0') {
        return;
    }

    fd = sceIoOpen(sOotPspVideoPath, PSP_O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    readSize = sceIoRead(fd, buffer, sizeof(buffer) - 1);
    sceIoClose(fd);
    if (readSize < 0) {
        return;
    }
    buffer[readSize] = '\0';

    for (line = buffer; line != NULL; line = nextLine) {
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
        line = OotPspVideo_Trim(line);
        if ((line[0] == '\0') || (line[0] == '[')) {
            continue;
        }
        equals = strchr(line, '=');
        if (equals == NULL) {
            continue;
        }
        *equals = '\0';
        key = OotPspVideo_Trim(line);
        value = OotPspVideo_Trim(equals + 1);

        if (OotPspVideo_StrIcmp(key, "output") == 0) {
            sOotPspVideoOutput = (OotPspVideo_StrIcmp(value, "tv") == 0) ? OOT_PSP_VIDEO_OUTPUT_TV
                                                                          : OOT_PSP_VIDEO_OUTPUT_LCD;
        } else if (OotPspVideo_StrIcmp(key, "resolution") == 0) {
            if (OotPspVideo_StrIcmp(value, "480i") == 0) {
                sOotPspVideoResolution = OOT_PSP_VIDEO_RESOLUTION_480I;
            } else if (OotPspVideo_StrIcmp(value, "240p") == 0) {
                sOotPspVideoResolution = OOT_PSP_VIDEO_RESOLUTION_240P;
            } else {
                sOotPspVideoResolution = OOT_PSP_VIDEO_RESOLUTION_480P;
            }
        } else if (OotPspVideo_StrIcmp(key, "aspect") == 0) {
            sOotPspVideoAspect = (OotPspVideo_StrIcmp(value, "4:3") == 0) ? OOT_PSP_VIDEO_ASPECT_4_3
                                                                          : OOT_PSP_VIDEO_ASPECT_16_9;
        } else if (OotPspVideo_StrIcmp(key, "vsync") == 0) {
            OotPspVideo_SetVSyncEnabled((OotPspVideo_StrIcmp(value, "off") != 0) &&
                                      (OotPspVideo_StrIcmp(value, "false") != 0) &&
                                      (OotPspVideo_StrIcmp(value, "no") != 0) &&
                                      (OotPspVideo_StrIcmp(value, "0") != 0));
        }
    }

    if (!OotPspVideo_IsTvAvailable()) {
        sOotPspVideoOutput = OOT_PSP_VIDEO_OUTPUT_LCD;
    }
    if ((OotPspDve_GetCable() == OOT_PSP_DVE_CABLE_COMPOSITE) &&
        (sOotPspVideoResolution == OOT_PSP_VIDEO_RESOLUTION_480P)) {
        sOotPspVideoResolution = OOT_PSP_VIDEO_RESOLUTION_480I;
    }
    if (sOotPspVideoResolution == OOT_PSP_VIDEO_RESOLUTION_240P) {
        sOotPspVideoAspect = OOT_PSP_VIDEO_ASPECT_4_3;
    }
    OotPspVideo_CommitMode();
}

int OotPspVideo_Save(void) {
    char buffer[192];
    const char* resolutionName;
    int length;
    int written;
    SceUID fd;

    if (sOotPspVideoPath[0] == '\0') {
        return -1;
    }

    resolutionName = (sOotPspVideoResolution == OOT_PSP_VIDEO_RESOLUTION_480I)
                         ? "480i"
                         : ((sOotPspVideoResolution == OOT_PSP_VIDEO_RESOLUTION_240P) ? "240p" : "480p");
    length = snprintf(buffer, sizeof(buffer), "[video]\noutput = %s\nresolution = %s\naspect = %s\nvsync = %s\n",
                      (sOotPspVideoOutput == OOT_PSP_VIDEO_OUTPUT_TV) ? "tv" : "lcd", resolutionName,
                      (sOotPspVideoAspect == OOT_PSP_VIDEO_ASPECT_4_3) ? "4:3" : "16:9",
                      sOotPspVideoVSync ? "on" : "off");
    if ((length < 0) || ((size_t)length >= sizeof(buffer))) {
        return -1;
    }

    fd = sceIoOpen(sOotPspVideoPath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        return fd;
    }
    written = sceIoWrite(fd, buffer, length);
    sceIoClose(fd);
    return (written == length) ? 0 : -1;
}

int OotPspVideo_IsTvAvailable(void) {
    return (OotPspDve_GetModel() > 0) && OotPspDve_IsAvailable();
}

int OotPspVideo_RefreshTvAvailability(void) {
    if (!OotPspVideo_IsTvAvailable()) {
        return 0;
    }
    (void)OotPspDve_RefreshCable();
    return 1;
}

int OotPspVideo_IsTvSelected(void) {
    return sOotPspVideoOutput == OOT_PSP_VIDEO_OUTPUT_TV;
}

void OotPspVideo_CycleOutput(int direction) {
    (void)direction;
    if (OotPspVideo_RefreshTvAvailability()) {
        sOotPspVideoOutput = (sOotPspVideoOutput == OOT_PSP_VIDEO_OUTPUT_LCD) ? OOT_PSP_VIDEO_OUTPUT_TV
                                                                              : OOT_PSP_VIDEO_OUTPUT_LCD;
        if ((sOotPspVideoOutput == OOT_PSP_VIDEO_OUTPUT_TV) &&
            (OotPspDve_GetCable() == OOT_PSP_DVE_CABLE_COMPOSITE) &&
            (sOotPspVideoResolution == OOT_PSP_VIDEO_RESOLUTION_480P)) {
            sOotPspVideoResolution = OOT_PSP_VIDEO_RESOLUTION_480I;
        }
        if ((sOotPspVideoOutput == OOT_PSP_VIDEO_OUTPUT_TV) &&
            (sOotPspVideoResolution == OOT_PSP_VIDEO_RESOLUTION_240P)) {
            sOotPspVideoAspect = OOT_PSP_VIDEO_ASPECT_4_3;
        }
    } else {
        sOotPspVideoOutput = OOT_PSP_VIDEO_OUTPUT_LCD;
    }
}

void OotPspVideo_CycleResolution(int direction) {
    int modeCount;
    int resolution;

    if (sOotPspVideoOutput != OOT_PSP_VIDEO_OUTPUT_TV) {
        return;
    }

    /* Composite supports the two CRT modes; component also supports 480p. */
    modeCount = (OotPspDve_GetCable() == OOT_PSP_DVE_CABLE_COMPOSITE) ? 2 : 3;
    resolution = (int)sOotPspVideoResolution;
    if ((resolution < 0) || (resolution >= modeCount)) {
        resolution = 0;
    }
    resolution = (resolution + ((direction < 0) ? modeCount - 1 : 1)) % modeCount;
    sOotPspVideoResolution = (OotPspVideoResolution)resolution;
    if (sOotPspVideoResolution == OOT_PSP_VIDEO_RESOLUTION_240P) {
        sOotPspVideoAspect = OOT_PSP_VIDEO_ASPECT_4_3;
    }
}

void OotPspVideo_CycleAspect(int direction) {
    (void)direction;
    if (sOotPspVideoResolution == OOT_PSP_VIDEO_RESOLUTION_240P) {
        return;
    }
    sOotPspVideoAspect = (sOotPspVideoAspect == OOT_PSP_VIDEO_ASPECT_4_3) ? OOT_PSP_VIDEO_ASPECT_16_9
                                                                          : OOT_PSP_VIDEO_ASPECT_4_3;
}

int OotPspVideo_IsVSyncEnabled(void) {
    return sOotPspVideoVSync;
}

void OotPspVideo_SetVSyncEnabled(int enabled) {
    sOotPspVideoVSync = enabled != 0;
}

const char* OotPspVideo_GetOutputName(void) {
    if (OotPspDve_GetModel() <= 0) {
        return "LCD (PSP-1000)";
    }
    if (!OotPspDve_IsAvailable()) {
        return "LCD (DVE load failed)";
    }
    return (sOotPspVideoOutput == OOT_PSP_VIDEO_OUTPUT_TV) ? "TV" : "LCD";
}

const char* OotPspVideo_GetResolutionName(void) {
    if (sOotPspVideoOutput == OOT_PSP_VIDEO_OUTPUT_LCD) {
        return "480x272";
    }
    if (sOotPspVideoResolution == OOT_PSP_VIDEO_RESOLUTION_240P) {
        return "320x240p (CRT)";
    }
    if ((OotPspDve_GetCable() == OOT_PSP_DVE_CABLE_COMPOSITE) ||
        (sOotPspVideoResolution == OOT_PSP_VIDEO_RESOLUTION_480I)) {
        return "720x480i";
    }
    return "720x480p";
}

const char* OotPspVideo_GetAspectName(void) {
    if ((sOotPspVideoOutput == OOT_PSP_VIDEO_OUTPUT_TV) &&
        (sOotPspVideoResolution == OOT_PSP_VIDEO_RESOLUTION_240P)) {
        return "4:3 (fixed)";
    }
    return (sOotPspVideoAspect == OOT_PSP_VIDEO_ASPECT_4_3) ? "4:3" : "16:9";
}

static void OotPspVideo_ResolveMode(OotPspVideoOutput output, OotPspVideoResolution resolution,
                                    OotPspVideoAspect aspect, OotPspVideoMode* mode) {
    int useTv = (output == OOT_PSP_VIDEO_OUTPUT_TV) && OotPspVideo_IsTvAvailable();
    int useCrt240p = resolution == OOT_PSP_VIDEO_RESOLUTION_240P;
    int useInterlaced = (resolution != OOT_PSP_VIDEO_RESOLUTION_480P) ||
                        (OotPspDve_GetCable() == OOT_PSP_DVE_CABLE_COMPOSITE);

    memset(mode, 0, sizeof(*mode));
    mode->tvOutput = useTv;
    if (!useTv) {
        mode->bufferWidth = 512;
        mode->displayWidth = 480;
        mode->displayHeight = 272;
        mode->crt240p = 0;
        mode->viewportWidth = (aspect == OOT_PSP_VIDEO_ASPECT_4_3) ? 362 : 480;
        mode->viewportHeight = 272;
        mode->dveOutput = 0;
        mode->dveMode = 0;
        mode->dveWidth = 480;
        mode->dveHeight = 272;
        return;
    }

    mode->bufferWidth = useCrt240p ? 512 : 768;
    mode->displayWidth = useCrt240p ? 320 : 720;
    mode->displayHeight = useCrt240p ? 240 : 480;
    mode->crt240p = useCrt240p;
    mode->viewportWidth = useCrt240p ? 320 : ((aspect == OOT_PSP_VIDEO_ASPECT_4_3) ? 640 : 720);
    mode->viewportHeight = useCrt240p ? 240 : ((aspect == OOT_PSP_VIDEO_ASPECT_4_3) ? 448 : 460);
    mode->dveOutput = (OotPspDve_GetCable() == OOT_PSP_DVE_CABLE_COMPOSITE) ? 2 : 0;
    mode->dveMode = useInterlaced ? 0x1D1 : 0x1D2;
    mode->dveWidth = useCrt240p ? 320 : 720;
    mode->dveHeight = useCrt240p ? 240 : (useInterlaced ? 503 : 480);
}

void OotPspVideo_GetMode(OotPspVideoMode* mode) {
    OotPspVideo_ResolveMode(sOotPspVideoOutput, sOotPspVideoResolution, sOotPspVideoAspect, mode);
}

void OotPspVideo_GetActiveMode(OotPspVideoMode* mode) {
    OotPspVideo_ResolveMode(sOotPspVideoActiveOutput, sOotPspVideoActiveResolution, sOotPspVideoActiveAspect, mode);
}
