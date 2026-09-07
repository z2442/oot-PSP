#ifndef OOT_PSP_MESSAGE_TABLES_H
#define OOT_PSP_MESSAGE_TABLES_H

#include "oot_psp_asset_loader.h"

s32 OotPspMessageTables_LoadActive(void);
void OotPspMessageTables_Resolve(const OotPspMessageEntry** entries, size_t* count);

#endif
