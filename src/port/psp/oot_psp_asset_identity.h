#ifndef OOT_PSP_ASSET_IDENTITY_H
#define OOT_PSP_ASSET_IDENTITY_H

#define OOT_PSP_PACKED_ASSET_PATH         "data/segments/oot_psp_assets.bin"
#define OOT_PSP_PACKED_ASSET_TEMP_PATH    "data/segments/oot_psp_assets.tmp"
#define OOT_PSP_PACKED_ASSET_ID_PATH      "data/segments/oot_psp_assets.id"
#define OOT_PSP_PACKED_ASSET_ID_TEMP_PATH "data/segments/oot_psp_assets.id.tmp"

#define OOT_PSP_ASSET_ID_MAGIC          "OPID"
#define OOT_PSP_SCENE_ASSET_MAX          512U
#define OOT_PSP_SCENE_VROM_BASE          0x50000000U
#define OOT_PSP_SCENE_VROM_STRIDE        0x00100000U
#define OOT_PSP_ASSET_ID_SIZE           (128U + OOT_PSP_SCENE_ASSET_MAX * 16U)
#define OOT_PSP_ASSET_ID_VERSION        14U
#define OOT_PSP_ASSET_ID_PACKED_SIZE    8U
#define OOT_PSP_ASSET_ID_PROFILE_OFFSET 32U
#define OOT_PSP_ASSET_ID_PROFILE_SIZE   32U

#endif
