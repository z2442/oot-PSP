#ifndef OOT_PSP_VFPU_MATRIX_H
#define OOT_PSP_VFPU_MATRIX_H

#include "ultra64.h"
#include "z_math.h"

static inline u32 OotPspVfpu_FloatBits(f32 value) {
    union {
        f32 f;
        u32 u;
    } bits = { value };

    return bits.u;
}

/*
 * Compute both values together: vrot shares the VFPU's angle reduction and
 * avoids two software libm calls for the common matrix-rotation case.
 */
static inline void OotPspVfpu_SinCos(f32 radians, f32* sinOut, f32* cosOut) {
    u32 radiansBits = OotPspVfpu_FloatBits(radians);

    __asm__ volatile(
        "mtv %[radians], S002\n"
        "vcst.s S003, VFPU_2_PI\n"
        "vmul.s S002, S002, S003\n"
        "vrot.p C000, S002, [s, c]\n"
        "sv.s S000, 0(%[sinOut])\n"
        "sv.s S001, 0(%[cosOut])\n"
        :
        : [radians] "r"(radiansBits), [sinOut] "r"(sinOut), [cosOut] "r"(cosOut)
        : "memory");
}

static inline void OotPspVfpu_MtxFIdentity(MtxF* dest) {
    __asm__ volatile(
        "vmidt.q M000\n"
        "sv.s S000, 0(%[dest])\n"
        "sv.s S001, 4(%[dest])\n"
        "sv.s S002, 8(%[dest])\n"
        "sv.s S003, 12(%[dest])\n"
        "sv.s S010, 16(%[dest])\n"
        "sv.s S011, 20(%[dest])\n"
        "sv.s S012, 24(%[dest])\n"
        "sv.s S013, 28(%[dest])\n"
        "sv.s S020, 32(%[dest])\n"
        "sv.s S021, 36(%[dest])\n"
        "sv.s S022, 40(%[dest])\n"
        "sv.s S023, 44(%[dest])\n"
        "sv.s S030, 48(%[dest])\n"
        "sv.s S031, 52(%[dest])\n"
        "sv.s S032, 56(%[dest])\n"
        "sv.s S033, 60(%[dest])\n"
        :
        : [dest] "r"(dest)
        : "memory");
}

static inline void OotPspVfpu_MtxFSetScale(MtxF* dest, f32 x, f32 y, f32 z) {
    u32 xBits = OotPspVfpu_FloatBits(x);
    u32 yBits = OotPspVfpu_FloatBits(y);
    u32 zBits = OotPspVfpu_FloatBits(z);

    __asm__ volatile(
        "vmidt.q M000\n"
        "mtv %[x], S000\n"
        "mtv %[y], S011\n"
        "mtv %[z], S022\n"
        "sv.s S000, 0(%[dest])\n"
        "sv.s S001, 4(%[dest])\n"
        "sv.s S002, 8(%[dest])\n"
        "sv.s S003, 12(%[dest])\n"
        "sv.s S010, 16(%[dest])\n"
        "sv.s S011, 20(%[dest])\n"
        "sv.s S012, 24(%[dest])\n"
        "sv.s S013, 28(%[dest])\n"
        "sv.s S020, 32(%[dest])\n"
        "sv.s S021, 36(%[dest])\n"
        "sv.s S022, 40(%[dest])\n"
        "sv.s S023, 44(%[dest])\n"
        "sv.s S030, 48(%[dest])\n"
        "sv.s S031, 52(%[dest])\n"
        "sv.s S032, 56(%[dest])\n"
        "sv.s S033, 60(%[dest])\n"
        :
        : [dest] "r"(dest), [x] "r"(xBits), [y] "r"(yBits), [z] "r"(zBits)
        : "memory");
}

static inline void OotPspVfpu_MtxFSetTranslate(MtxF* dest, f32 x, f32 y, f32 z) {
    u32 xBits = OotPspVfpu_FloatBits(x);
    u32 yBits = OotPspVfpu_FloatBits(y);
    u32 zBits = OotPspVfpu_FloatBits(z);

    __asm__ volatile(
        "vmidt.q M000\n"
        "mtv %[x], S030\n"
        "mtv %[y], S031\n"
        "mtv %[z], S032\n"
        "sv.s S000, 0(%[dest])\n"
        "sv.s S001, 4(%[dest])\n"
        "sv.s S002, 8(%[dest])\n"
        "sv.s S003, 12(%[dest])\n"
        "sv.s S010, 16(%[dest])\n"
        "sv.s S011, 20(%[dest])\n"
        "sv.s S012, 24(%[dest])\n"
        "sv.s S013, 28(%[dest])\n"
        "sv.s S020, 32(%[dest])\n"
        "sv.s S021, 36(%[dest])\n"
        "sv.s S022, 40(%[dest])\n"
        "sv.s S023, 44(%[dest])\n"
        "sv.s S030, 48(%[dest])\n"
        "sv.s S031, 52(%[dest])\n"
        "sv.s S032, 56(%[dest])\n"
        "sv.s S033, 60(%[dest])\n"
        :
        : [dest] "r"(dest), [x] "r"(xBits), [y] "r"(yBits), [z] "r"(zBits)
        : "memory");
}

static inline void OotPspVfpu_MtxFCopy(MtxF* dest, const MtxF* src) {
    s32 i;

    for (i = 0; i < 16; i++) {
        ((f32*)dest)[i] = ((const f32*)src)[i];
    }
}

static inline void OotPspVfpu_MtxFMult(const MtxF* a, const MtxF* b, MtxF* dest) {
    __asm__ volatile(
        "lv.s S000, 0(%[a])\n"
        "lv.s S001, 4(%[a])\n"
        "lv.s S002, 8(%[a])\n"
        "lv.s S003, 12(%[a])\n"
        "lv.s S010, 16(%[a])\n"
        "lv.s S011, 20(%[a])\n"
        "lv.s S012, 24(%[a])\n"
        "lv.s S013, 28(%[a])\n"
        "lv.s S020, 32(%[a])\n"
        "lv.s S021, 36(%[a])\n"
        "lv.s S022, 40(%[a])\n"
        "lv.s S023, 44(%[a])\n"
        "lv.s S030, 48(%[a])\n"
        "lv.s S031, 52(%[a])\n"
        "lv.s S032, 56(%[a])\n"
        "lv.s S033, 60(%[a])\n"
        "lv.s S100, 0(%[b])\n"
        "lv.s S101, 4(%[b])\n"
        "lv.s S102, 8(%[b])\n"
        "lv.s S103, 12(%[b])\n"
        "lv.s S110, 16(%[b])\n"
        "lv.s S111, 20(%[b])\n"
        "lv.s S112, 24(%[b])\n"
        "lv.s S113, 28(%[b])\n"
        "lv.s S120, 32(%[b])\n"
        "lv.s S121, 36(%[b])\n"
        "lv.s S122, 40(%[b])\n"
        "lv.s S123, 44(%[b])\n"
        "lv.s S130, 48(%[b])\n"
        "lv.s S131, 52(%[b])\n"
        "lv.s S132, 56(%[b])\n"
        "lv.s S133, 60(%[b])\n"
        "vmmul.q M200, M000, M100\n"
        "sv.s S200, 0(%[dest])\n"
        "sv.s S201, 4(%[dest])\n"
        "sv.s S202, 8(%[dest])\n"
        "sv.s S203, 12(%[dest])\n"
        "sv.s S210, 16(%[dest])\n"
        "sv.s S211, 20(%[dest])\n"
        "sv.s S212, 24(%[dest])\n"
        "sv.s S213, 28(%[dest])\n"
        "sv.s S220, 32(%[dest])\n"
        "sv.s S221, 36(%[dest])\n"
        "sv.s S222, 40(%[dest])\n"
        "sv.s S223, 44(%[dest])\n"
        "sv.s S230, 48(%[dest])\n"
        "sv.s S231, 52(%[dest])\n"
        "sv.s S232, 56(%[dest])\n"
        "sv.s S233, 60(%[dest])\n"
        :
        : [a] "r"(a), [b] "r"(b), [dest] "r"(dest)
        : "memory");
}

static inline void OotPspVfpu_MtxFMultVec3f(const MtxF* mf, const Vec3f* src, Vec3f* dest) {
    __asm__ volatile(
        "lv.s S100, 0(%[src])\n"
        "lv.s S101, 4(%[src])\n"
        "lv.s S102, 8(%[src])\n"
        "vone.s S103\n"
        "lv.s S000, 0(%[mf])\n"
        "lv.s S001, 16(%[mf])\n"
        "lv.s S002, 32(%[mf])\n"
        "lv.s S003, 48(%[mf])\n"
        "lv.s S010, 4(%[mf])\n"
        "lv.s S011, 20(%[mf])\n"
        "lv.s S012, 36(%[mf])\n"
        "lv.s S013, 52(%[mf])\n"
        "lv.s S020, 8(%[mf])\n"
        "lv.s S021, 24(%[mf])\n"
        "lv.s S022, 40(%[mf])\n"
        "lv.s S023, 56(%[mf])\n"
        "vdot.q S200, C000, C100\n"
        "vdot.q S201, C010, C100\n"
        "vdot.q S202, C020, C100\n"
        "sv.s S200, 0(%[dest])\n"
        "sv.s S201, 4(%[dest])\n"
        "sv.s S202, 8(%[dest])\n"
        :
        : [mf] "r"(mf), [src] "r"(src), [dest] "r"(dest)
        : "memory");
}

static inline void OotPspVfpu_MtxFMultVec3fW(const MtxF* mf, const Vec3f* src, Vec3f* dest, f32* wDest) {
    __asm__ volatile(
        "lv.s S100, 0(%[src])\n"
        "lv.s S101, 4(%[src])\n"
        "lv.s S102, 8(%[src])\n"
        "vone.s S103\n"
        "lv.s S000, 0(%[mf])\n"
        "lv.s S001, 16(%[mf])\n"
        "lv.s S002, 32(%[mf])\n"
        "lv.s S003, 48(%[mf])\n"
        "lv.s S010, 4(%[mf])\n"
        "lv.s S011, 20(%[mf])\n"
        "lv.s S012, 36(%[mf])\n"
        "lv.s S013, 52(%[mf])\n"
        "lv.s S020, 8(%[mf])\n"
        "lv.s S021, 24(%[mf])\n"
        "lv.s S022, 40(%[mf])\n"
        "lv.s S023, 56(%[mf])\n"
        "lv.s S030, 12(%[mf])\n"
        "lv.s S031, 28(%[mf])\n"
        "lv.s S032, 44(%[mf])\n"
        "lv.s S033, 60(%[mf])\n"
        "vdot.q S200, C000, C100\n"
        "vdot.q S201, C010, C100\n"
        "vdot.q S202, C020, C100\n"
        "vdot.q S203, C030, C100\n"
        "sv.s S200, 0(%[dest])\n"
        "sv.s S201, 4(%[dest])\n"
        "sv.s S202, 8(%[dest])\n"
        "sv.s S203, 0(%[wDest])\n"
        :
        : [mf] "r"(mf), [src] "r"(src), [dest] "r"(dest), [wDest] "r"(wDest)
        : "memory");
}

static inline void OotPspVfpu_MtxFTranslate(MtxF* mf, f32 x, f32 y, f32 z) {
    u32 xBits = OotPspVfpu_FloatBits(x);
    u32 yBits = OotPspVfpu_FloatBits(y);
    u32 zBits = OotPspVfpu_FloatBits(z);

    __asm__ volatile(
        "mtv %[x], S100\n"
        "mtv %[y], S101\n"
        "mtv %[z], S102\n"
        "vone.s S103\n"
        "lv.s S000, 0(%[mf])\n"
        "lv.s S001, 16(%[mf])\n"
        "lv.s S002, 32(%[mf])\n"
        "lv.s S003, 48(%[mf])\n"
        "lv.s S010, 4(%[mf])\n"
        "lv.s S011, 20(%[mf])\n"
        "lv.s S012, 36(%[mf])\n"
        "lv.s S013, 52(%[mf])\n"
        "lv.s S020, 8(%[mf])\n"
        "lv.s S021, 24(%[mf])\n"
        "lv.s S022, 40(%[mf])\n"
        "lv.s S023, 56(%[mf])\n"
        "lv.s S030, 12(%[mf])\n"
        "lv.s S031, 28(%[mf])\n"
        "lv.s S032, 44(%[mf])\n"
        "lv.s S033, 60(%[mf])\n"
        "vdot.q S200, C000, C100\n"
        "vdot.q S201, C010, C100\n"
        "vdot.q S202, C020, C100\n"
        "vdot.q S203, C030, C100\n"
        "sv.s S200, 48(%[mf])\n"
        "sv.s S201, 52(%[mf])\n"
        "sv.s S202, 56(%[mf])\n"
        "sv.s S203, 60(%[mf])\n"
        :
        : [mf] "r"(mf), [x] "r"(xBits), [y] "r"(yBits), [z] "r"(zBits)
        : "memory");
}

static inline void OotPspVfpu_MtxFScale(MtxF* mf, f32 x, f32 y, f32 z) {
    u32 xBits = OotPspVfpu_FloatBits(x);
    u32 yBits = OotPspVfpu_FloatBits(y);
    u32 zBits = OotPspVfpu_FloatBits(z);

    __asm__ volatile(
        "lv.s S000, 0(%[mf])\n"
        "lv.s S001, 4(%[mf])\n"
        "lv.s S002, 8(%[mf])\n"
        "lv.s S003, 12(%[mf])\n"
        "lv.s S010, 16(%[mf])\n"
        "lv.s S011, 20(%[mf])\n"
        "lv.s S012, 24(%[mf])\n"
        "lv.s S013, 28(%[mf])\n"
        "lv.s S020, 32(%[mf])\n"
        "lv.s S021, 36(%[mf])\n"
        "lv.s S022, 40(%[mf])\n"
        "lv.s S023, 44(%[mf])\n"
        "mtv %[x], S100\n"
        "mtv %[y], S101\n"
        "mtv %[z], S102\n"
        "vscl.q C000, C000, S100\n"
        "vscl.q C010, C010, S101\n"
        "vscl.q C020, C020, S102\n"
        "sv.s S000, 0(%[mf])\n"
        "sv.s S001, 4(%[mf])\n"
        "sv.s S002, 8(%[mf])\n"
        "sv.s S003, 12(%[mf])\n"
        "sv.s S010, 16(%[mf])\n"
        "sv.s S011, 20(%[mf])\n"
        "sv.s S012, 24(%[mf])\n"
        "sv.s S013, 28(%[mf])\n"
        "sv.s S020, 32(%[mf])\n"
        "sv.s S021, 36(%[mf])\n"
        "sv.s S022, 40(%[mf])\n"
        "sv.s S023, 44(%[mf])\n"
        :
        : [mf] "r"(mf), [x] "r"(xBits), [y] "r"(yBits), [z] "r"(zBits)
        : "memory");
}

static inline void OotPspVfpu_MtxFRotateX(MtxF* mf, f32 sin, f32 cos) {
    u32 sinBits = OotPspVfpu_FloatBits(sin);
    u32 cosBits = OotPspVfpu_FloatBits(cos);

    __asm__ volatile(
        "lv.s S000, 16(%[mf])\n"
        "lv.s S001, 20(%[mf])\n"
        "lv.s S002, 24(%[mf])\n"
        "lv.s S003, 28(%[mf])\n"
        "lv.s S010, 32(%[mf])\n"
        "lv.s S011, 36(%[mf])\n"
        "lv.s S012, 40(%[mf])\n"
        "lv.s S013, 44(%[mf])\n"
        "mtv %[sin], S200\n"
        "mtv %[cos], S201\n"
        "vscl.q C020, C000, S201\n"
        "vscl.q C030, C010, S200\n"
        "vadd.q C020, C020, C030\n"
        "vscl.q C030, C010, S201\n"
        "vscl.q C100, C000, S200\n"
        "vsub.q C030, C030, C100\n"
        "sv.s S020, 16(%[mf])\n"
        "sv.s S021, 20(%[mf])\n"
        "sv.s S022, 24(%[mf])\n"
        "sv.s S023, 28(%[mf])\n"
        "sv.s S030, 32(%[mf])\n"
        "sv.s S031, 36(%[mf])\n"
        "sv.s S032, 40(%[mf])\n"
        "sv.s S033, 44(%[mf])\n"
        :
        : [mf] "r"(mf), [sin] "r"(sinBits), [cos] "r"(cosBits)
        : "memory");
}

static inline void OotPspVfpu_MtxFRotateY(MtxF* mf, f32 sin, f32 cos) {
    u32 sinBits = OotPspVfpu_FloatBits(sin);
    u32 cosBits = OotPspVfpu_FloatBits(cos);

    __asm__ volatile(
        "lv.s S000, 0(%[mf])\n"
        "lv.s S001, 4(%[mf])\n"
        "lv.s S002, 8(%[mf])\n"
        "lv.s S003, 12(%[mf])\n"
        "lv.s S010, 32(%[mf])\n"
        "lv.s S011, 36(%[mf])\n"
        "lv.s S012, 40(%[mf])\n"
        "lv.s S013, 44(%[mf])\n"
        "mtv %[sin], S200\n"
        "mtv %[cos], S201\n"
        "vscl.q C020, C000, S201\n"
        "vscl.q C030, C010, S200\n"
        "vsub.q C020, C020, C030\n"
        "vscl.q C030, C000, S200\n"
        "vscl.q C100, C010, S201\n"
        "vadd.q C030, C030, C100\n"
        "sv.s S020, 0(%[mf])\n"
        "sv.s S021, 4(%[mf])\n"
        "sv.s S022, 8(%[mf])\n"
        "sv.s S023, 12(%[mf])\n"
        "sv.s S030, 32(%[mf])\n"
        "sv.s S031, 36(%[mf])\n"
        "sv.s S032, 40(%[mf])\n"
        "sv.s S033, 44(%[mf])\n"
        :
        : [mf] "r"(mf), [sin] "r"(sinBits), [cos] "r"(cosBits)
        : "memory");
}

static inline void OotPspVfpu_MtxFRotateZ(MtxF* mf, f32 sin, f32 cos) {
    u32 sinBits = OotPspVfpu_FloatBits(sin);
    u32 cosBits = OotPspVfpu_FloatBits(cos);

    __asm__ volatile(
        "lv.s S000, 0(%[mf])\n"
        "lv.s S001, 4(%[mf])\n"
        "lv.s S002, 8(%[mf])\n"
        "lv.s S003, 12(%[mf])\n"
        "lv.s S010, 16(%[mf])\n"
        "lv.s S011, 20(%[mf])\n"
        "lv.s S012, 24(%[mf])\n"
        "lv.s S013, 28(%[mf])\n"
        "mtv %[sin], S200\n"
        "mtv %[cos], S201\n"
        "vscl.q C020, C000, S201\n"
        "vscl.q C030, C010, S200\n"
        "vadd.q C020, C020, C030\n"
        "vscl.q C030, C010, S201\n"
        "vscl.q C100, C000, S200\n"
        "vsub.q C030, C030, C100\n"
        "sv.s S020, 0(%[mf])\n"
        "sv.s S021, 4(%[mf])\n"
        "sv.s S022, 8(%[mf])\n"
        "sv.s S023, 12(%[mf])\n"
        "sv.s S030, 16(%[mf])\n"
        "sv.s S031, 20(%[mf])\n"
        "sv.s S032, 24(%[mf])\n"
        "sv.s S033, 28(%[mf])\n"
        :
        : [mf] "r"(mf), [sin] "r"(sinBits), [cos] "r"(cosBits)
        : "memory");
}

static inline void OotPspVfpu_MtxFReplaceRotation(MtxF* dest, const MtxF* rotation) {
    __asm__ volatile(
        "lv.s S000, 0(%[dest])\n"
        "lv.s S001, 4(%[dest])\n"
        "lv.s S002, 8(%[dest])\n"
        "lv.s S010, 16(%[dest])\n"
        "lv.s S011, 20(%[dest])\n"
        "lv.s S012, 24(%[dest])\n"
        "lv.s S020, 32(%[dest])\n"
        "lv.s S021, 36(%[dest])\n"
        "lv.s S022, 40(%[dest])\n"
        "lv.s S100, 0(%[rotation])\n"
        "lv.s S101, 4(%[rotation])\n"
        "lv.s S102, 8(%[rotation])\n"
        "lv.s S110, 16(%[rotation])\n"
        "lv.s S111, 20(%[rotation])\n"
        "lv.s S112, 24(%[rotation])\n"
        "lv.s S120, 32(%[rotation])\n"
        "lv.s S121, 36(%[rotation])\n"
        "lv.s S122, 40(%[rotation])\n"
        "vdot.t S200, C000, C000\n"
        "vdot.t S201, C010, C010\n"
        "vdot.t S202, C020, C020\n"
        "vsqrt.s S200, S200\n"
        "vsqrt.s S201, S201\n"
        "vsqrt.s S202, S202\n"
        "vscl.t C100, C100, S200\n"
        "vscl.t C110, C110, S201\n"
        "vscl.t C120, C120, S202\n"
        "sv.s S100, 0(%[dest])\n"
        "sv.s S101, 4(%[dest])\n"
        "sv.s S102, 8(%[dest])\n"
        "sv.s S110, 16(%[dest])\n"
        "sv.s S111, 20(%[dest])\n"
        "sv.s S112, 24(%[dest])\n"
        "sv.s S120, 32(%[dest])\n"
        "sv.s S121, 36(%[dest])\n"
        "sv.s S122, 40(%[dest])\n"
        :
        : [dest] "r"(dest), [rotation] "r"(rotation)
        : "memory");
}

#endif
