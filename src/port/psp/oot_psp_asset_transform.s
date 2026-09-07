.section .rodata
.balign 16

#ifndef OOT_PSP_ASSET_TRANSFORM_PATH
#define OOT_PSP_ASSET_TRANSFORM_PATH "assets/psp/ntsc-1.0/asset_transform.z"
#endif

.global gOotPspAssetTransformCompressed
gOotPspAssetTransformCompressed:
.incbin OOT_PSP_ASSET_TRANSFORM_PATH

.global gOotPspAssetTransformCompressedEnd
gOotPspAssetTransformCompressedEnd:
