#include "mem.h"

#include <malloc.h>
#include <unistd.h>

// We leave the heap split to libctru. It claims all of the app's memory at startup anyway,
// so choosing our own split saves nothing. Shrinking the linear heap is actively harmful:
// the normal heap gets the remainder, which in 124 MB mode overflows its 96 MB address range,
// and libctru panics before main() even runs. Setting __ctru_linear_heap_size to 24 MB did
// exactly that on a real console.

extern char* fake_heap_end;   // end of the normal heap, set up by libctru

u32 mem_linear_free(void) { return linearSpaceFree(); }

u32 mem_heap_free(void)
{
	struct mallinfo info = mallinfo();
	char* top = (char*)sbrk(0);
	u32 unclaimed = fake_heap_end > top ? (u32)(fake_heap_end - top) : 0;
	return (u32)info.fordblks + unclaimed;
}
