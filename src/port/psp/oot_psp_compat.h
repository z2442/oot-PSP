#ifndef OOT_PSP_COMPAT_H
#define OOT_PSP_COMPAT_H

#include <stddef.h>

#include "assert.h"

#if defined(TARGET_PSP)
int OotPsp_IsSystemHeapRange(const void* ptr, size_t size);
int OotPsp_IsRuntimeByteRange(const void* ptr, size_t size);
#endif

#endif
