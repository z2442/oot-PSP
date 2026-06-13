#ifndef OOT_PSP_COMPAT_H
#define OOT_PSP_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#include "assert.h"

#if defined(TARGET_PSP)
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
