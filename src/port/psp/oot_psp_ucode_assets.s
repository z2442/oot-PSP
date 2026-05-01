.section .rodata
.balign 16

.globl njpgdspMainTextStart
njpgdspMainTextStart:
    .incbin "incbin/njpgdspMainText"
.balign 16
.globl njpgdspMainTextEnd
njpgdspMainTextEnd:

.globl njpgdspMainDataStart
njpgdspMainDataStart:
    .incbin "incbin/njpgdspMainData"
.balign 16
.globl njpgdspMainDataEnd
njpgdspMainDataEnd:
