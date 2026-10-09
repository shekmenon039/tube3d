#include "net.h"

#include <malloc.h>
#include <stdlib.h>

#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x80000   // 512 KB is plenty for one stream plus thumbnails; the usual 1 MB wastes memory

static u32* soc_buffer = NULL;

bool net_init(void)
{
	soc_buffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
	if (!soc_buffer) return false;
	if (R_FAILED(socInit(soc_buffer, SOC_BUFFERSIZE)))
	{
		free(soc_buffer);
		soc_buffer = NULL;
		return false;
	}
	return true;
}

void net_exit(void)
{
	if (soc_buffer)
	{
		socExit();
		free(soc_buffer);
		soc_buffer = NULL;
	}
}
