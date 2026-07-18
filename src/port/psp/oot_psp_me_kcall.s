    .set noreorder

#include "pspstub.s"

/*
 * libme-core's generated kcall import descriptor is made entirely of local
 * symbols.  The port links with --gc-sections, so that descriptor can be
 * discarded even though its three stub functions remain referenced.  Keep a
 * global root for the same import table used by the working me-core samples.
 */
    .section .rodata.sceResident, "a"
    .word 0
.LootPspMeKcallModuleName:
    .asciz "kcall"
    .align 2

    .section .lib.stub, "a", @progbits
    .globl ootPspMeKcallImport
    .type ootPspMeKcallImport, @object
ootPspMeKcallImport:
    .word .LootPspMeKcallModuleName
    .word 0x40090000
    .word 0x00030005
    .word .LootPspMeKcallNids
    .word .LootPspMeKcallStubs
    .size ootPspMeKcallImport, . - ootPspMeKcallImport

    .section .rodata.sceNid, "a"
.LootPspMeKcallNids:

    .section .sceStub.text, "ax", @progbits
.LootPspMeKcallStubs:

    STUB_FUNC 0x75F43FF0, kcall_2
    STUB_FUNC 0xBADD8D3B, kcall_3
    STUB_FUNC 0x2BB46CB6, kinit

