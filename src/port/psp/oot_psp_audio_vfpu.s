.set noat

.text
.set push
.set noreorder

/*
 * Allegrex VFPU kernels for the ABI mixer.  Samples are unpacked from signed
 * 16-bit PCM into four-wide float vectors, processed in parallel, saturated,
 * and packed back to PCM.  All public counts are multiples of eight samples.
 */

/* void OotPspAudioVfpu_Add(s16* out, const s16* in, s32 samples); */
.globl OotPspAudioVfpu_Add
OotPspAudioVfpu_Add:
    blez    $a2, .Ladd_done
    nop

    /* The kernels operate on column vectors so lv.q/sv.q can move eight
     * packed PCM samples at a time. */
    li      $t0, -32768
    li      $t1, 32767
    mtv     $t0, S620
    mtv     $t1, S630
    vi2f.s  S620, S620, 0
    vi2f.s  S630, S630, 0
    vone.q  C600
    vone.q  C610
    vscl.q  C600, C600, S620
    vscl.q  C610, C610, S630

.Ladd_loop:
    lv.q    C000, 0($a1)
    lv.q    C010, 0($a0)
    vs2i.p  C100, C000
    vs2i.p  C110, C002
    vs2i.p  C200, C010
    vs2i.p  C210, C012
    vi2f.q  C100, C100, 16
    vi2f.q  C110, C110, 16
    vi2f.q  C200, C200, 16
    vi2f.q  C210, C210, 16

    vadd.q  C200, C200, C100
    vadd.q  C210, C210, C110
    vmax.q  C200, C200, C600
    vmax.q  C210, C210, C600
    vmin.q  C200, C200, C610
    vmin.q  C210, C210, C610
    vf2iz.q C200, C200, 16
    vf2iz.q C210, C210, 16
    vi2s.q  C300, C200
    vi2s.q  C302, C210
    sv.q    C300, 0($a0)

    addiu   $a0, $a0, 16
    addiu   $a1, $a1, 16
    addiu   $a2, $a2, -8
    bgtz    $a2, .Ladd_loop
    nop

.Ladd_done:
    jr      $ra
    nop

/*
 * void OotPspAudioVfpu_Mix(s16* out, const s16* in, s32 gain,
 *                          s32 samples);
 *
 * Implements clamp(floor(out * 32767/32768 + in * gain/32768 + 0.5)).
 */
.globl OotPspAudioVfpu_Mix
OotPspAudioVfpu_Mix:
    blez    $a3, .Lmix_done
    nop

    li      $t0, 32767
    mtv     $a2, S420
    mtv     $t0, S430
    vi2f.s  S420, S420, 15
    vi2f.s  S430, S430, 15
    vone.q  C400
    vone.q  C410
    vscl.q  C400, C400, S420
    vscl.q  C410, C410, S430

    li      $t0, -32768
    li      $t1, 32767
    mtv     $t0, S620
    mtv     $t1, S630
    vi2f.s  S620, S620, 0
    vi2f.s  S630, S630, 0
    vone.q  C600
    vone.q  C610
    vscl.q  C600, C600, S620
    vscl.q  C610, C610, S630

.Lmix_loop:
    lv.q    C000, 0($a1)
    lv.q    C010, 0($a0)
    vs2i.p  C100, C000
    vs2i.p  C110, C002
    vs2i.p  C200, C010
    vs2i.p  C210, C012
    vi2f.q  C100, C100, 16
    vi2f.q  C110, C110, 16
    vi2f.q  C200, C200, 16
    vi2f.q  C210, C210, 16

    vmul.q  C100, C100, C400
    vmul.q  C110, C110, C400
    vmul.q  C200, C200, C410
    vmul.q  C210, C210, C410
    vadd.q  C200, C200, C100
    vadd.q  C210, C210, C110
    vadd.q  C200, C200, C200[1/2,1/2,1/2,1/2]
    vadd.q  C210, C210, C210[1/2,1/2,1/2,1/2]
    vmax.q  C200, C200, C600
    vmax.q  C210, C210, C600
    vmin.q  C200, C200, C610
    vmin.q  C210, C210, C610
    vf2id.q C200, C200, 16
    vf2id.q C210, C210, 16
    vi2s.q  C300, C200
    vi2s.q  C302, C210
    sv.q    C300, 0($a0)

    addiu   $a0, $a0, 16
    addiu   $a1, $a1, 16
    addiu   $a3, $a3, -8
    bgtz    $a3, .Lmix_loop
    nop

.Lmix_done:
    jr      $ra
    nop

/*
 * OotPspAudioVfpuEnvArgs layout:
 *   0x00 in, 0x04 dryLeft, 0x08 dryRight, 0x0c wetLeft,
 *   0x10 wetRight,
 *   0x14 volLeft0,  0x18 volRight0, 0x1c reverb0,
 *   0x20 volLeft1,  0x24 volRight1, 0x28 reverb1.
 *
 * The caller handles the rare headset XOR-mask path scalarly.  The two
 * volume sets cover one 16-sample ABI envelope block.  Each loop iteration
 * moves eight packed samples with aligned 128-bit loads and stores.
 */
.globl OotPspAudioVfpu_EnvMix16
OotPspAudioVfpu_EnvMix16:
    move    $t8, $a0
    lw      $t0, 0x00($a0)
    lw      $t1, 0x04($a0)
    lw      $t2, 0x08($a0)
    lw      $t3, 0x0c($a0)
    lw      $t4, 0x10($a0)

    li      $t5, -32768
    li      $t6, 32767
    mtv     $t5, S620
    mtv     $t6, S630
    vi2f.s  S620, S620, 0
    vi2f.s  S630, S630, 0
    vone.q  C600
    vone.q  C610
    vscl.q  C600, C600, S620
    vscl.q  C610, C610, S630

    li      $t9, 2
.Lenv_half_loop:
    lw      $t5, 0x14($t8)
    lw      $t6, 0x18($t8)
    lw      $t7, 0x1c($t8)
    mtv     $t5, S700
    mtv     $t6, S701
    mtv     $t7, S702
    vi2f.s  S700, S700, 16
    vi2f.s  S701, S701, 16
    vi2f.s  S702, S702, 16

    lv.q    C000, 0($t0)
    vs2i.p  C100, C000
    vs2i.p  C110, C002
    vi2f.q  C100, C100, 16
    vi2f.q  C110, C110, 16

    /* Left dry products, rounded down to signed high-half results. */
    vscl.q  C200, C100, S700
    vscl.q  C210, C110, S700
    vf2id.q C200, C200, 0
    vf2id.q C210, C210, 0
    vi2f.q  C200, C200, 0
    vi2f.q  C210, C210, 0

    /* Saturating dry-left accumulation. */
    lv.q    C010, 0($t1)
    vs2i.p  C300, C010
    vs2i.p  C310, C012
    vi2f.q  C300, C300, 16
    vi2f.q  C310, C310, 16
    vadd.q  C300, C300, C200
    vadd.q  C310, C310, C210
    vmax.q  C300, C300, C600
    vmax.q  C310, C310, C600
    vmin.q  C300, C300, C610
    vmin.q  C310, C310, C610
    vf2iz.q C300, C300, 16
    vf2iz.q C310, C310, 16
    vi2s.q  C400, C300
    vi2s.q  C402, C310
    sv.q    C400, 0($t1)

    /* Wet-left products and accumulation. */
    vscl.q  C200, C200, S702
    vscl.q  C210, C210, S702
    vf2id.q C200, C200, 0
    vf2id.q C210, C210, 0
    vi2f.q  C200, C200, 0
    vi2f.q  C210, C210, 0
    lv.q    C010, 0($t3)
    vs2i.p  C300, C010
    vs2i.p  C310, C012
    vi2f.q  C300, C300, 16
    vi2f.q  C310, C310, 16
    vadd.q  C300, C300, C200
    vadd.q  C310, C310, C210
    vmax.q  C300, C300, C600
    vmax.q  C310, C310, C600
    vmin.q  C300, C300, C610
    vmin.q  C310, C310, C610
    vf2iz.q C300, C300, 16
    vf2iz.q C310, C310, 16
    vi2s.q  C400, C300
    vi2s.q  C402, C310
    sv.q    C400, 0($t3)

    /* Right dry products, rounded down to signed high-half results. */
    vscl.q  C200, C100, S701
    vscl.q  C210, C110, S701
    vf2id.q C200, C200, 0
    vf2id.q C210, C210, 0
    vi2f.q  C200, C200, 0
    vi2f.q  C210, C210, 0

    /* Saturating dry-right accumulation. */
    lv.q    C010, 0($t2)
    vs2i.p  C300, C010
    vs2i.p  C310, C012
    vi2f.q  C300, C300, 16
    vi2f.q  C310, C310, 16
    vadd.q  C300, C300, C200
    vadd.q  C310, C310, C210
    vmax.q  C300, C300, C600
    vmax.q  C310, C310, C600
    vmin.q  C300, C300, C610
    vmin.q  C310, C310, C610
    vf2iz.q C300, C300, 16
    vf2iz.q C310, C310, 16
    vi2s.q  C400, C300
    vi2s.q  C402, C310
    sv.q    C400, 0($t2)

    /* Wet-right products and accumulation. */
    vscl.q  C200, C200, S702
    vscl.q  C210, C210, S702
    vf2id.q C200, C200, 0
    vf2id.q C210, C210, 0
    vi2f.q  C200, C200, 0
    vi2f.q  C210, C210, 0
    lv.q    C010, 0($t4)
    vs2i.p  C300, C010
    vs2i.p  C310, C012
    vi2f.q  C300, C300, 16
    vi2f.q  C310, C310, 16
    vadd.q  C300, C300, C200
    vadd.q  C310, C310, C210
    vmax.q  C300, C300, C600
    vmax.q  C310, C310, C600
    vmin.q  C300, C300, C610
    vmin.q  C310, C310, C610
    vf2iz.q C300, C300, 16
    vf2iz.q C310, C310, 16
    vi2s.q  C400, C300
    vi2s.q  C402, C310
    sv.q    C400, 0($t4)

    addiu   $t0, $t0, 16
    addiu   $t1, $t1, 16
    addiu   $t2, $t2, 16
    addiu   $t3, $t3, 16
    addiu   $t4, $t4, 16
    addiu   $t8, $t8, 12
    addiu   $t9, $t9, -1
    bgtz    $t9, .Lenv_half_loop
    nop

.Lenv_done:
    jr      $ra
    nop

.set pop
