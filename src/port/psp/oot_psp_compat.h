#ifndef OOT_PSP_COMPAT_H
#define OOT_PSP_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#include "assert.h"
#include "math.h"

#if defined(TARGET_PSP)
#ifndef OOT_PSP_FAST_SQRT
#define OOT_PSP_FAST_SQRT 1
#endif

#if OOT_PSP_FAST_SQRT
/* newlib's sqrtf performs a full software IEEE-754 square root on Allegrex.
 * The game does not consume errno from math calls, so use the PSP's native
 * single-precision sqrt instruction instead.  Keep this inline to avoid a
 * call and return in collision and actor-update hot paths. */
static inline __attribute__((always_inline)) float OotPsp_Sqrtf(float value) {
    float result;

    __asm__("sqrt.s %0, %1" : "=f"(result) : "f"(value));
    return result;
}
#define sqrtf OotPsp_Sqrtf
/* The game's double sqrt call sites all operate on f32 geometry and consume
 * an f32 result. Allegrex has no native double-precision square root. */
#define sqrt(value) OotPsp_Sqrtf((float)(value))
#endif

int OotPsp_IsSystemHeapRange(const void* ptr, size_t size);

extern unsigned char __bss_start[];
extern unsigned char _end[];

int OotPsp_IsRuntimeByteRangeSlow(uintptr_t start, uintptr_t end) __attribute__((noinline));

static inline int OotPsp_IsRuntimeByteRange(const void* ptr, size_t size) {
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end;

    if ((ptr == NULL) || (size == 0) || (start > UINTPTR_MAX - size)) {
        return 0;
    }

    end = start + size;
    if ((start >= (uintptr_t)__bss_start) && (end <= (uintptr_t)_end)) {
        return 1;
    }

    return OotPsp_IsRuntimeByteRangeSlow(start, end);
}
#endif

#endif
