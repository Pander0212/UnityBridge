// SPDX-License-Identifier: GPL-3.0-or-later
// Apple platforms have no <malloc.h>; crunch's crn_decomp.h includes it for malloc_usable_size (M8).
#pragma once
#include <stdlib.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#define malloc_usable_size malloc_size
#else
#include_next <malloc.h>
#endif
