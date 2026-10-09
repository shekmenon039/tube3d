// See uifont.h. The atlas format is described in tools/make_font.py.
#include "uifont.h"

#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef UIFONT_FILE
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"   // the generated header tests __cplusplus
#include "ui_font_bin.h"                   // data/ui_font.bin
#pragma GCC diagnostic pop
#endif

#define BASE_PX           24.0f   // atlas pixel size that matches citro2d text scale 1.0 (make_font.py)
#define MAX_SIZES         24
#define FILE_HEADER_SIZE  12   // "T3F1", atlas width and height, size count, glyph count
#define SIZE_RECORD_SIZE  6    // one Size in the file
#define GLYPH_RECORD_SIZE 12   // one Glyph in the file

typedef struct {
	u16 cp, x, y;
	u8 w, h;
	s8 left, top;   // bitmap position relative to the pen on the baseline
	u8 adv;
} Glyph;

typedef struct {
	u8 px;
	u16 first, count;   // glyphs[first..first+count), sorted by codepoint
} Size;

static C3D_Tex tex;
static bool ready = false;
static Glyph* glyphs = NULL;
static Size sizes[MAX_SIZES];
static int size_count = 0;
static float sys_ascent = 23.0f, sys_line = 30.0f;   // system font metrics at scale 1

static u16 rd16(const u8* p) { return (u16)(p[0] | p[1] << 8); }

bool uifont_init(void)
{
	if (ready) return true;
	const u8* data;
	size_t len;
#ifdef UIFONT_FILE
	// the PC preview build reads the atlas from disk
	static u8* file_data = NULL;
	FILE* f = fopen(UIFONT_FILE, "rb");
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
	data = ui_font_bin;
	len = ui_font_bin_size;
#endif
	if (len < FILE_HEADER_SIZE || memcmp(data, "T3F1", 4) != 0) return false;
	u16 atlas_w = rd16(data + 4), atlas_h = rd16(data + 6);
	int file_sizes = rd16(data + 8), file_glyphs = rd16(data + 10);
	size_t need = FILE_HEADER_SIZE + (size_t)file_sizes * SIZE_RECORD_SIZE + (size_t)file_glyphs * GLYPH_RECORD_SIZE +
	              (size_t)atlas_w * atlas_h;
	if (file_sizes > MAX_SIZES || len < need || atlas_w < 8 || atlas_h < 8) return false;
	const u8* p = data + FILE_HEADER_SIZE;
	for (int i = 0; i < file_sizes; i++, p += SIZE_RECORD_SIZE)
	{
		sizes[i].px = p[0];
		sizes[i].first = rd16(p + 2);
		sizes[i].count = rd16(p + 4);
		if (sizes[i].first + sizes[i].count > file_glyphs) return false;
	}
	glyphs = (Glyph*)malloc((size_t)file_glyphs * sizeof(Glyph));
	if (!glyphs) return false;
	for (int i = 0; i < file_glyphs; i++, p += GLYPH_RECORD_SIZE)
	{
		Glyph* g = &glyphs[i];
		g->cp = rd16(p);
		g->x = rd16(p + 2);
		g->y = rd16(p + 4);
		g->w = p[6];
		g->h = p[7];
		g->left = (s8)p[8];
		g->top = (s8)p[9];
		g->adv = p[10];
	}
	if (!C3D_TexInit(&tex, atlas_w, atlas_h, GPU_A8))
	{
		free(glyphs);
		glyphs = NULL;
		return false;
	}
	memcpy(tex.data, p, (size_t)atlas_w * atlas_h);   // already in the GPU's tiled layout
	C3D_TexFlush(&tex);
	// glyphs are drawn 1:1 at whole pixels, so nearest sampling reproduces each one exactly
	C3D_TexSetFilter(&tex, GPU_NEAREST, GPU_NEAREST);
	C3D_TexSetWrap(&tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	size_count = file_sizes;
	// place lines the way the system font does, so layouts made for it keep their look
	fontEnsureMapped();
	FINF_s* fi = fontGetInfo(NULL);
	if (fi && fi->ascent && fi->lineFeed)
	{
		sys_ascent = fi->ascent;
		sys_line = fi->lineFeed;
	}
	ready = true;
	return true;
}

bool uifont_ready(void) { return ready; }

void uifont_exit(void)
{
	if (!ready) return;
	C3D_TexDelete(&tex);
	free(glyphs);
	glyphs = NULL;
	ready = false;
}

static const Size* pick_size(float scale)
{
	float want = scale * BASE_PX;
	const Size* best = NULL;
	for (int i = 0; i < size_count; i++)
		if (!best || fabsf(sizes[i].px - want) < fabsf(best->px - want)) best = &sizes[i];
	return best;
}

static const Glyph* find_glyph(const Size* s, u32 cp)
{
	int lo = s->first, hi = s->first + s->count - 1;
	while (lo <= hi)
	{
		int mid = (lo + hi) / 2;
		if (glyphs[mid].cp == cp) return &glyphs[mid];
		if (glyphs[mid].cp < cp) lo = mid + 1;
		else hi = mid - 1;
	}
	return NULL;
}

// Reads the next UTF-8 codepoint of [*s, end). A broken sequence gives 0xFFFD, which the
// atlas doesn't have, so the string falls back to the system font.
static u32 next_cp(const char** s, const char* end)
{
	const u8* p = (const u8*)*s;
	u32 c = p[0];
	int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
	if (n < 0 || (const char*)p + n >= end)
	{
		(*s)++;
		return 0xFFFD;
	}
	if (n) c &= 0x3F >> n;
	for (int i = 1; i <= n; i++)
	{
		if ((p[i] & 0xC0) != 0x80)
		{
			(*s)++;
			return 0xFFFD;
		}
		c = (c << 6) | (p[i] & 0x3F);
	}
	*s += n + 1;
	return c;
}

// Width of [s, end) in pixels; false if a character is missing
static bool run_width(const Size* sz, const char* s, const char* end, int* w)
{
	int x = 0;
	while (s < end)
	{
		const Glyph* g = find_glyph(sz, next_cp(&s, end));
		if (!g) return false;
		x += g->adv;
	}
	*w = x;
	return true;
}

static void run_draw(const Size* sz, const char* s, const char* end, int x, int baseline, float z, u32 color)
{
	C2D_ImageTint tint;
	C2D_PlainImageTint(&tint, color, 1.0f);   // the colour, with the glyph's coverage as alpha
	float tw = tex.width, th = tex.height;
	while (s < end)
	{
		const Glyph* g = find_glyph(sz, next_cp(&s, end));
		if (!g) return;
		if (g->w && g->h)
		{
			Tex3DS_SubTexture sub = {
				.width = g->w,
				.height = g->h,
				.left = g->x / tw,
				.right = (g->x + g->w) / tw,
				.top = 1.0f - g->y / th,
				.bottom = 1.0f - (g->y + g->h) / th,
			};
			C2D_Image img = { &tex, &sub };
			C2D_DrawImageAt(img, (float)(x + g->left), (float)(baseline + g->top), z, &tint, 1.0f, 1.0f);
		}
		x += g->adv;
	}
}

static int baseline_of(float y, float scale) { return (int)lroundf(y + sys_ascent * scale); }

// Left edge, in whole pixels, of a run w wide aligned at x
static int aligned_left(float x, int w, int align)
{
	float left = align == UIF_CENTER ? x - w / 2.0f : align == UIF_RIGHT ? x - w : x;
	return (int)lroundf(left);
}

bool uifont_width(const char* s, float scale, float* w)
{
	if (!ready || !s) return false;
	int px;
	if (!run_width(pick_size(scale), s, s + strlen(s), &px)) return false;
	*w = (float)px;
	return true;
}

bool uifont_draw(const char* s, float x, float y, float z, float scale, uint32_t color, int align)
{
	if (!ready || !s) return false;
	const Size* sz = pick_size(scale);
	const char* end = s + strlen(s);
	int w;
	if (!run_width(sz, s, end, &w)) return false;
	run_draw(sz, s, end, aligned_left(x, w, align), baseline_of(y, scale), z, color);
	return true;
}

// The system font's line feed, so wrapped layouts keep its spacing
static float line_height(float scale) { return roundf(sys_line * scale); }

int uifont_draw_wrap(const char* s, float x, float y, float z, float scale, uint32_t color, int align, float width)
{
	if (!ready || !s) return 0;
	const Size* sz = pick_size(scale);
	const char* end = s + strlen(s);
	int unused_w;
	for (const char* p = s; p < end;)   // check every line's characters are in the atlas first
	{
		const char* line_end = p;
		while (line_end < end && *line_end != '\n') line_end++;
		if (!run_width(sz, p, line_end, &unused_w)) return 0;
		p = line_end < end ? line_end + 1 : line_end;
	}
	const int line_h = (int)line_height(scale);
	int base = baseline_of(y, scale), lines = 0;
	const char* p = s;
	while (p < end)
	{
		// the longest run of whole words from p that fits the width
		const char* brk = p;
		while (brk < end && *brk != '\n')
		{
			const char* word_end = brk;
			while (word_end < end && *word_end == ' ') word_end++;
			while (word_end < end && *word_end != ' ' && *word_end != '\n') word_end++;
			int run_w = 0;
			run_width(sz, p, word_end, &run_w);
			if (run_w > width) break;
			brk = word_end;
		}
		if (brk == p && p < end && *p != '\n')
		{
			// a single word wider than the line: take as many characters as fit, at least one
			next_cp(&brk, end);
			while (brk < end && *brk != ' ' && *brk != '\n')
			{
				const char* next = brk;
				next_cp(&next, end);
				int run_w = 0;
				run_width(sz, p, next, &run_w);
				if (run_w > width) break;
				brk = next;
			}
		}
		const char* draw_end = brk;   // trailing spaces don't count for the alignment
		while (draw_end > p && draw_end[-1] == ' ') draw_end--;
		int w;
		run_width(sz, p, draw_end, &w);
		run_draw(sz, p, draw_end, aligned_left(x, w, align), base, z, color);
		lines++;
		base += line_h;
		p = brk;   // always moves forward, unless we're at a '\n', which is skipped below
		while (p < end && *p == ' ') p++;
		if (p < end && *p == '\n') p++;
	}
	return lines;
}
