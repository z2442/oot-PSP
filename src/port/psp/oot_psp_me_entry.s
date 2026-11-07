#include "oot_psp_me_entry.h"

    .set noreorder
    .set noat
    .section .text.meLibOnProcess, "ax", @progbits
    .align 2
    .globl meLibOnProcess
    .type meLibOnProcess, @function
    .ent meLibOnProcess
meLibOnProcess:
    /* No C frame or saved stack data exists here. Shutdown published live
     * game data before reset; inherited firmware cache tags can be discarded
     * without writing unknown dirty lines back onto the post-wake bus. */
    lui     $k0, %hi(gOotPspAudioMeSharedStorage)
    addiu   $k0, $k0, %lo(gOotPspAudioMeSharedStorage)
    li      $k1, 0xA0000000
    or      $k0, $k0, $k1
    li      $k1, OOT_PSP_ME_ENTRY_CACHE_STAGE
    sw      $k1, OOT_PSP_ME_ENTRY_PROGRESS_OFFSET($k0)
    sync

    /* Allegrex index tag invalidation, as used by iplsdk's cache reset.
     * Read the configured index span rather than assuming an 8 KiB cache. */
    mfc0    $t0, $16
    ext     $t0, $t0, 6, 3
    li      $t2, 0x1000
    sllv    $t2, $t2, $t0
    mtc0    $zero, $28
    mtc0    $zero, $29
    sync
    move    $t1, $zero
.LclearDataTags:
    cache   0x11, 0($t1)
    cache   0x13, 0($t1)
    addiu   $t1, $t1, 64
    bne     $t1, $t2, .LclearDataTags
    nop
    sync

    /* Retain me-core's stack. The C worker is the first code to use it after
     * tag invalidation, and never returns into the discarded caller frame. */
    lui     $t9, %hi(OotPspAudioBackend_RunMe)
    addiu   $t9, $t9, %lo(OotPspAudioBackend_RunMe)
    li      $t0, 0x80000000
    or      $t9, $t9, $t0
    jr      $t9
    nop
    .end meLibOnProcess
    .size meLibOnProcess, . - meLibOnProcess
