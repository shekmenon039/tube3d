#pragma once
// Memory layout and live readouts.
#include <3ds.h>

// Bytes still free in the linear heap (MVD, framebuffers, textures, DSP buffers).
u32 mem_linear_free(void);

// Bytes still free in the normal heap (malloc).
u32 mem_heap_free(void);
