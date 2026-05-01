#ifndef SEGMENTED_ADDRESS_H
#define SEGMENTED_ADDRESS_H

#include "ultra64.h"
#include "stdint.h"

extern uintptr_t gSegments[NUM_SEGMENTS];

#if PLATFORM_PSP
void* SegmentedToVirtualCompat(uintptr_t addr);
#define SEGMENTED_TO_VIRTUAL(addr) SegmentedToVirtualCompat((uintptr_t)(addr))
#else
#define SEGMENTED_TO_VIRTUAL(addr) (void*)(gSegments[SEGMENT_NUMBER(addr)] + SEGMENT_OFFSET(addr) + K0BASE)
#endif

#endif
