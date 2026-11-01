#ifndef OOT_PSP_VIDEO_H
#define OOT_PSP_VIDEO_H

#include <stddef.h>

#define OOT_PSP_VIDEO_INI_PATH "video.ini"

typedef enum OotPspVideoOutput {
    OOT_PSP_VIDEO_OUTPUT_LCD = 0,
    OOT_PSP_VIDEO_OUTPUT_TV = 1,
} OotPspVideoOutput;

typedef enum OotPspVideoResolution {
    OOT_PSP_VIDEO_RESOLUTION_480I = 0,
    OOT_PSP_VIDEO_RESOLUTION_240P = 1,
    OOT_PSP_VIDEO_RESOLUTION_480P = 2,
} OotPspVideoResolution;

typedef enum OotPspVideoAspect {
    OOT_PSP_VIDEO_ASPECT_4_3 = 0,
    OOT_PSP_VIDEO_ASPECT_16_9 = 1,
} OotPspVideoAspect;

typedef struct OotPspVideoMode {
    int tvOutput;
    int bufferWidth;
    int displayWidth;
    int displayHeight;
    int crt240p;
    int viewportWidth;
    int viewportHeight;
    int dveOutput;
    int dveMode;
    int dveWidth;
    int dveHeight;
} OotPspVideoMode;

void OotPspVideo_Init(const char* executablePath);
void OotPspVideo_ResetDefaults(void);
void OotPspVideo_ForceLcd(void);
void OotPspVideo_CommitMode(void);
int OotPspVideo_Save(void);

int OotPspVideo_IsTvAvailable(void);
int OotPspVideo_RefreshTvAvailability(void);
int OotPspVideo_IsTvSelected(void);
void OotPspVideo_CycleOutput(int direction);
void OotPspVideo_CycleResolution(int direction);
void OotPspVideo_CycleAspect(int direction);
int OotPspVideo_IsVSyncEnabled(void);
void OotPspVideo_SetVSyncEnabled(int enabled);
const char* OotPspVideo_GetOutputName(void);
const char* OotPspVideo_GetResolutionName(void);
const char* OotPspVideo_GetAspectName(void);
void OotPspVideo_GetMode(OotPspVideoMode* mode);
void OotPspVideo_GetActiveMode(OotPspVideoMode* mode);

#endif
