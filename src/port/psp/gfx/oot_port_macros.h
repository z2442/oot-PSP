#ifndef OOT_PORT_PSP_GFX_OOT_PORT_MACROS_H
#define OOT_PORT_PSP_GFX_OOT_PORT_MACROS_H

#include "array_count.h"

#ifdef __GNUC__
#define UNUSED __attribute__((unused))
#else
#define UNUSED
#endif

#define _UNUSED(x) (void)(x)

#define ALIGN(VAL_, ALIGNMENT_) (((VAL_) + ((ALIGNMENT_) - 1)) & ~((ALIGNMENT_) - 1))

#endif
