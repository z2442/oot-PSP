#ifndef OOT_PSP_UNPACKER_UI_H
#define OOT_PSP_UNPACKER_UI_H
#include <stdbool.h>
#include <stdint.h>
void OotPspUnpackerUI_Draw(uint32_t progress, const char* status, bool error);
void OotPspUnpackerUI_Shutdown(void);
#endif
