// See logo.h. The file format is described in tools/make_logo.py.
#include "logo.h"

#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LOGO_FILE
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"   // the generated header tests __cplusplus
#include "logo_bin.h"                      // data/logo.bin
#pragma GCC diagnostic pop
#endif

#define MAX_IMAGES        8
#define FILE_HEADER_SIZE  12   // "T3L1", texture width and height, image count
#define IMAGE_RECORD_SIZE 18   // one LogoImage in the file

typedef struct {
	u8 size, layer;
	u16 x, y, w, h;   // in the texture
	s16 dx, dy;       // relative to the logo's top-left
	u16 logo_w, logo_h;
} LogoImage;

static C3D_Tex tex;
static LogoImage images[MAX_IMAGES];
static int image_count = 0;
static bool ready = false;

static u16 rd16(const u8* p) { return (u16)(p[0] | p[1] << 8); }

bool logo_init(void)
{
	if (ready) return true;
	const u8* data;
	size_t len;
#ifdef LOGO_FILE
	// the PC preview build reads it from disk
	static u8* file_data = NULL;
	FILE* f = fopen(LOGO_FILE, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long file_size = ftell(f);
	fseek(f, 0, SEEK_SET);
	file_data = (u8*)malloc((size_t)file_size);
	if (!file_data || fread(file_data, 1, (size_t)file_size, f) != (size_t)file_size)
	{
		fclose(f);
		return false;
	}
	fclose(f);
	data = file_data;
	len = (size_t)file_size;
#else
	data = logo_bin;
	len = logo_bin_size;
#endif
	if (len < FILE_HEADER_SIZE || memcmp(data, "T3L1", 4) != 0) return false;
	const u16 tex_w = rd16(data + 4), tex_h = rd16(data + 6);
	const int count = rd16(data + 8);
	const size_t need = FILE_HEADER_SIZE + (size_t)count * IMAGE_RECORD_SIZE + (size_t)tex_w * tex_h * 4;
	if (count > MAX_IMAGES || len < need || tex_w < 8 || tex_h < 8) return false;
	const u8* p = data + FILE_HEADER_SIZE;
	for (int i = 0; i < count; i++, p += IMAGE_RECORD_SIZE)
	{
		LogoImage* im = &images[i];
		im->size = p[0];
		im->layer = p[1];
		im->x = rd16(p + 2);
		im->y = rd16(p + 4);
		im->w = rd16(p + 6);
		im->h = rd16(p + 8);
		im->dx = (s16)rd16(p + 10);
		im->dy = (s16)rd16(p + 12);
		im->logo_w = rd16(p + 14);
		im->logo_h = rd16(p + 16);
	}
	if (!C3D_TexInit(&tex, tex_w, tex_h, GPU_RGBA8)) return false;
	memcpy(tex.data, p, (size_t)tex_w * tex_h * 4);   // already in the GPU's tiled layout
	C3D_TexFlush(&tex);
	C3D_TexSetFilter(&tex, GPU_NEAREST, GPU_NEAREST);   // it's drawn 1:1 at whole pixels
	C3D_TexSetWrap(&tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	image_count = count;
	ready = true;
	return true;
}

void logo_exit(void)
{
	if (!ready) return;
	C3D_TexDelete(&tex);
	ready = false;
	image_count = 0;
}

void logo_size(int size, int* w, int* h)
{
	*w = *h = 0;
	for (int i = 0; ready && i < image_count; i++)
		if (images[i].size == size)
		{
			*w = images[i].logo_w;
			*h = images[i].logo_h;
			return;
		}
}

bool logo_draw(int size, float x, float y, float base_dx, float pop_dx)
{
	if (!ready) return false;
	bool any = false;
	for (int i = 0; i < image_count; i++)
	{
		const LogoImage* im = &images[i];
		if (im->size != size) continue;
		const float dx = im->layer ? pop_dx : base_dx;
		Tex3DS_SubTexture sub = {
			.width = im->w,
			.height = im->h,
			.left = im->x / (float)tex.width,
			.right = (im->x + im->w) / (float)tex.width,
			.top = 1.0f - im->y / (float)tex.height,
			.bottom = 1.0f - (im->y + im->h) / (float)tex.height,
		};
		C2D_Image img = { &tex, &sub };
		C2D_DrawImageAt(img, roundf(x + dx) + im->dx, roundf(y) + im->dy, 0.5f, NULL, 1.0f, 1.0f);
		any = true;
	}
	return any;
}
