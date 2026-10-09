#include "thumbs.h"
#include "stereo.h"
#include "yt.h"
#include "filethumb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>

#define FAIL_RETRY_MS 15000   // retry a failed thumbnail after this long
#define SLOTS         24      // 7 cards fit on screen; this is up to 3 MB of textures
#define TEX_W         256
#define TEX_H         128
#define THUMB_W       224   // the 16:9 picture area in the texture's top-left corner
#define THUMB_H       126
#define TEX_BYTES     (TEX_W * TEX_H * 2)
#define RESP_MAX      (128 * 1024)   // YouTube's mqdefault JPEGs are 10-40 KB
#define WORKER_STACK  (48 * 1024)

typedef enum { TH_EMPTY, TH_QUEUED, TH_LOADING, TH_PIXELS, TH_READY, TH_FAILED } ThumbState;

typedef struct {
	char id[24];
	char layout[4];
	ThumbState state;
	int priority;
	u32 last_used;
	u64 failed_at;   // when the download failed; it's retried after FAIL_RETRY_MS
	u32 avg;
	int count;       // 1 for 2D, 2 for a left and a right eye
	int disparity;   // 3D only: how far the right eye's picture sits from the left's (in 224-px units)
	u8* pixels[2];   // downloaded and waiting to be uploaded
	C3D_Tex tex[2];
	C2D_Image img[2];
	Tex3DS_SubTexture sub;
} Slot;

static Slot slots[SLOTS];
static LightLock lock;
static Thread worker = NULL;
static volatile bool quit = false;
static u32 frame_no = 0;
static char* resp = NULL;

// ---------------------------------------------------------------- textures
//
// Textures are RGB565 in the GPU's tiled layout: 8x8 tiles in rows starting from the top of
// the picture, with pixels in Morton order inside each tile. The picture fills the top-left
// 224x126 and the rest is padded with its average colour. The source is YouTube's own
// 320x180 JPEG, decoded on the console.

static u32 tile_offset(u32 x, u32 y)
{
	u32 morton = 0;
	for (int i = 0; i < 3; i++) morton |= ((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1);
	return ((y >> 3) * (TEX_W / 8) + (x >> 3)) * 64 + morton;
}

static u16 to565(u32 r, u32 g, u32 b) { return (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

// Scales a crop rectangle of an RGB image to 224x126 (bilinear) and writes it as a tiled texture.
static void make_texture(const u8* rgb, int w, int h, float cx, float cy, float cw, float chh, u8* out, u32* avg)
{
	u16* texels = (u16*)out;
	u32 sum_r = 0, sum_g = 0, sum_b = 0;
	u16 row[THUMB_W];
	for (int y = 0; y < THUMB_H; y++)
	{
		float fy = cy + (y + 0.5f) * chh / THUMB_H - 0.5f;
		int y0 = (int)fy;
		if (y0 < 0) y0 = 0;
		if (y0 > h - 2) y0 = h - 2;
		float wy = fy - y0;
		if (wy < 0) wy = 0;
		if (wy > 1) wy = 1;
		for (int x = 0; x < THUMB_W; x++)
		{
			float fx = cx + (x + 0.5f) * cw / THUMB_W - 0.5f;
			int x0 = (int)fx;
			if (x0 < 0) x0 = 0;
			if (x0 > w - 2) x0 = w - 2;
			float wx = fx - x0;
			if (wx < 0) wx = 0;
			if (wx > 1) wx = 1;
			const u8* upper = rgb + (y0 * w + x0) * 3;
			const u8* lower = upper + w * 3;
			u32 px[3];
			for (int k = 0; k < 3; k++)
			{
				float top = upper[k] + (upper[k + 3] - upper[k]) * wx;
				float bottom = lower[k] + (lower[k + 3] - lower[k]) * wx;
				px[k] = (u32)(top + (bottom - top) * wy + 0.5f);
			}
			sum_r += px[0];
			sum_g += px[1];
			sum_b += px[2];
			row[x] = to565(px[0], px[1], px[2]);
		}
		for (int x = 0; x < THUMB_W; x++) texels[tile_offset(x, y)] = row[x];
	}
	u32 n = THUMB_W * THUMB_H;
	u32 r = sum_r / n, g = sum_g / n, b = sum_b / n;
	if (avg) *avg = (r << 16) | (g << 8) | b;
	u16 fill = to565(r, g, b);   // pad with the average colour so filtering leaves no dark seams
	for (u32 y = 0; y < TEX_H; y++)
		for (u32 x = (y < THUMB_H ? THUMB_W : 0); x < TEX_W; x++) texels[tile_offset(x, y)] = fill;
}

// Estimates the shift d (in picture pixels) at which right(x + d) best matches left(x),
// searching +-24 px on a half-resolution luminance image. Moving each eye d/2 towards the
// other puts the main subject at the card's depth instead of stacking the video's own depth
// on top of it.
static int estimate_disparity(const u8* left_tex, const u8* right_tex, bool* pair)
{
	enum { W = THUMB_W / 2, H = THUMB_H / 2 };   // half resolution is plenty
	static u8 L[W * H], R[W * H];
	const u16* lt = (const u16*)left_tex;
	const u16* rt = (const u16*)right_tex;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
		{
			L[y * W + x] = stereo_luma565(lt[tile_offset(x * 2, y * 2)]);
			R[y * W + x] = stereo_luma565(rt[tile_offset(x * 2, y * 2)]);
		}
	return stereo_analyse(L, R, W, H, 12, pair) * 2;   // back to picture pixels
}

// Turns an RGB picture into textures: the whole picture for 2D, or each eye's half for 3D.
// Returns how many it made (0 if out of memory).
static int to_textures(const u8* rgb, int w, int h, const char* layout, u8* px[2], u32* avg)
{
	bool sbs = !strcmp(layout, "sbs"), tb = !strcmp(layout, "tb");
	int count = sbs || tb ? 2 : 1;
	for (int e = 0; e < count; e++)
	{
		px[e] = (u8*)malloc(TEX_BYTES);
		if (!px[e]) return 0;
		float cx = 0, cy = 0, cw = (float)w, chh = (float)h;
		if (sbs)
		{
			cw = w / 2.0f;
			cx = e ? cw : 0;
		}   // each eye's half, stretched back to 16:9
		if (tb)
		{
			chh = h / 2.0f;
			cy = e ? chh : 0;
		}
		make_texture(rgb, w, h, cx, cy, cw, chh, px[e], e == 0 ? avg : NULL);
	}
	return count;
}

// Downloads (or, for a local file, grabs a frame of) one thumbnail and converts it. Returns
// the number of textures (1 or 2), or 0 on failure.
static int fetch(void* http, const char* id, const char* layout, u8* px[2], u32* avg)
{
	if (id[0] == 'f' && strlen(id) == 17)   // a filethumb id is "f" plus 16 hex digits (YouTube ids have 11 characters)
	{
		// A file from the Videos tab: grab a frame (filethumb.c) once and keep it, so the "2d"
		// retry in worker_main can cut it again.
		static char last_id[24];
		static u8* last_rgb = NULL;
		static int last_w, last_h;
		if (strcmp(last_id, id) != 0)
		{
			free(last_rgb);
			last_rgb = NULL;
			last_id[0] = 0;
			if (!filethumb_frame(id, &last_rgb, &last_w, &last_h)) return 0;
			snprintf(last_id, sizeof(last_id), "%s", id);
		}
		if (!last_rgb) return 0;
		return to_textures(last_rgb, last_w, last_h, layout, px, avg);
	}
	char url[96];
	snprintf(url, sizeof(url), "https://i.ytimg.com/vi/%s/mqdefault.jpg", id);
	size_t len = 0;
	if (yt_http_get(http, url, (u8*)resp, RESP_MAX, &len) != 200 || len < 4) return 0;
	tjhandle tj = tjInitDecompress();
	if (!tj) return 0;
	int w = 0, h = 0, subsamp, colorspace, count = 0;
	u8* rgb = NULL;
	if (tjDecompressHeader3(tj, (u8*)resp, len, &w, &h, &subsamp, &colorspace) == 0 && w >= 16 && h >= 16 &&
	    w <= 1280 && h <= 720 && (rgb = (u8*)malloc(w * h * 3)) != NULL &&
	    tjDecompress2(tj, (u8*)resp, len, rgb, w, 0, h, TJPF_RGB, TJFLAG_FASTDCT) == 0)
		count = to_textures(rgb, w, h, layout, px, avg);
	free(rgb);
	tjDestroy(tj);
	return count;
}

static void worker_main(void* arg)
{
	void* http = yt_http_new();   // our own connection to i.ytimg.com
	while (!quit)
	{
		// pick the queued slot with the highest priority
		LightLock_Lock(&lock);
		int best = -1;
		for (int i = 0; i < SLOTS; i++)
			if (slots[i].state == TH_QUEUED && (best < 0 || slots[i].priority > slots[best].priority)) best = i;
		char id[24], layout[4];
		if (best >= 0)
		{
			slots[best].state = TH_LOADING;
			memcpy(id, slots[best].id, sizeof(id));
			memcpy(layout, slots[best].layout, sizeof(layout));
		}
		LightLock_Unlock(&lock);

		if (best < 0)
		{
			svcSleepThread(30 * 1000000ULL);
			continue;
		}

		u8* px[2] = { NULL, NULL };
		u32 avg = 0;
		int count = fetch(http, id, layout, px, &avg);

		bool pair = true;
		int disparity = count == 2 ? estimate_disparity(px[0], px[1], &pair) : 0;
		if (!pair)
		{
			// 3D videos often have an ordinary 2D thumbnail picked by the creator rather than a
			// video frame. Its halves aren't a stereo pair, so the card shows the whole picture
			// flat. The video itself still plays in 3D.
			free(px[0]);
			free(px[1]);
			px[0] = px[1] = NULL;
			disparity = 0;
			count = fetch(http, id, "2d", px, &avg);
		}

		LightLock_Lock(&lock);
		Slot* s = &slots[best];
		// the slot may have been handed to another video while we were downloading
		if (s->state == TH_LOADING && !strcmp(s->id, id) && !strcmp(s->layout, layout))
		{
			if (count > 0)
			{
				s->pixels[0] = px[0];
				s->pixels[1] = px[1];
				s->count = count;
				s->avg = avg;
				s->disparity = disparity;
				s->state = TH_PIXELS;
				px[0] = px[1] = NULL;
			}
			else
			{
				s->state = TH_FAILED;
				s->failed_at = osGetTime();
			}
		}
		LightLock_Unlock(&lock);
		free(px[0]);
		free(px[1]);
	}
	yt_http_free(http);
}

void thumbs_init(void)
{
	LightLock_Init(&lock);
	memset(slots, 0, sizeof(slots));
	resp = (char*)malloc(RESP_MAX);
	if (!resp) return;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// Runs below the UI's priority so downloads never make the menu stutter. It needs a 48 KB
	// stack because TLS handshakes (mbedTLS bignums) and JPEG decoding happen here.
	worker = threadCreate(worker_main, NULL, WORKER_STACK, prio + 1, -2, false);
}

// Frees what a slot holds. Call it on the main thread only, after taking the slot away from
// the worker under the lock (by setting it to TH_EMPTY). was is the state it had before that.
static void free_slot(Slot* s, ThumbState was)
{
	if (was == TH_READY)
		for (int i = 0; i < s->count; i++) C3D_TexDelete(&s->tex[i]);
	free(s->pixels[0]);
	free(s->pixels[1]);
	s->pixels[0] = s->pixels[1] = NULL;
	s->count = 0;
}

void thumbs_release(void)
{
	LightLock_Lock(&lock);
	for (int i = 0; i < SLOTS; i++)
	{
		ThumbState was = slots[i].state;
		if (was == TH_LOADING)
		{
			slots[i].state = TH_EMPTY;
			continue;
		}   // the worker will throw its result away
		slots[i].state = TH_EMPTY;
		free_slot(&slots[i], was);
	}
	LightLock_Unlock(&lock);
}

void thumbs_exit(void)
{
	quit = true;
	if (worker)
	{
		threadJoin(worker, U64_MAX);
		threadFree(worker);
		worker = NULL;
	}
	for (int i = 0; i < SLOTS; i++)
	{
		ThumbState was = slots[i].state;   // the worker has stopped
		slots[i].state = TH_EMPTY;
		free_slot(&slots[i], was);
	}
	free(resp);
	resp = NULL;
}

void thumbs_request(const char* id, const char* layout, int priority)
{
	if (!id || !*id || !worker) return;
	LightLock_Lock(&lock);
	int found = -1, victim = -1;
	for (int i = 0; i < SLOTS; i++)
	{
		Slot* s = &slots[i];
		if (s->state != TH_EMPTY && !strcmp(s->id, id) && !strcmp(s->layout, layout))
		{
			found = i;
			break;
		}
		// to recycle, prefer an empty slot, then the least recently used finished one
		if (s->state == TH_EMPTY)
		{
			if (victim < 0 || slots[victim].state != TH_EMPTY) victim = i;
		}
		else if ((s->state == TH_READY || s->state == TH_FAILED || s->state == TH_QUEUED) &&
		         s->last_used + 2 < frame_no)
		{
			if (victim < 0 || (slots[victim].state != TH_EMPTY && s->last_used < slots[victim].last_used)) victim = i;
		}
	}
	if (found >= 0)
	{
		Slot* s = &slots[found];
		s->last_used = frame_no;
		s->priority = priority;
		if (s->state == TH_FAILED && osGetTime() - s->failed_at > FAIL_RETRY_MS)
			s->state = TH_QUEUED;   // maybe it was a network hiccup; try again
	}
	else if (victim >= 0)
	{
		Slot* s = &slots[victim];
		// Take the slot away from the worker first (it only picks up TH_QUEUED slots), then
		// free its textures outside the lock, since that's GPU work for the main thread.
		ThumbState was = s->state;
		s->state = TH_EMPTY;
		LightLock_Unlock(&lock);
		free_slot(s, was);
		LightLock_Lock(&lock);
		snprintf(s->id, sizeof(s->id), "%s", id);
		snprintf(s->layout, sizeof(s->layout), "%s", layout);
		s->priority = priority;
		s->last_used = frame_no;
		s->state = TH_QUEUED;
	}
	LightLock_Unlock(&lock);
}

bool thumbs_update(void)
{
	frame_no++;
	int uploads = 0;

	for (int i = 0; i < SLOTS && uploads < 2; i++)
	{
		Slot* s = &slots[i];
		LightLock_Lock(&lock);
		bool ready = s->state == TH_PIXELS;
		LightLock_Unlock(&lock);
		if (!ready) continue;

		bool ok = true;
		for (int e = 0; e < s->count; e++)
		{
			if (!C3D_TexInit(&s->tex[e], TEX_W, TEX_H, GPU_RGB565))
			{
				for (int k = 0; k < e; k++) C3D_TexDelete(&s->tex[k]);
				ok = false;
				break;
			}
			memcpy(s->tex[e].data, s->pixels[e], TEX_BYTES);
			C3D_TexFlush(&s->tex[e]);
			C3D_TexSetFilter(&s->tex[e], GPU_LINEAR, GPU_LINEAR);
			C3D_TexSetWrap(&s->tex[e], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		}
		// the picture is the top-left 224x126 of the 256x128 texture, and v=1 is its top row
		s->sub.width = THUMB_W;
		s->sub.height = THUMB_H;
		s->sub.left = 0.0f;
		s->sub.top = 1.0f;
		s->sub.right = (float)THUMB_W / TEX_W;
		s->sub.bottom = 1.0f - (float)THUMB_H / TEX_H;
		for (int e = 0; e < 2; e++)
		{
			s->img[e].tex = &s->tex[e < s->count ? e : 0];
			s->img[e].subtex = &s->sub;
		}
		LightLock_Lock(&lock);
		free(s->pixels[0]);
		free(s->pixels[1]);
		s->pixels[0] = s->pixels[1] = NULL;
		s->state = ok ? TH_READY : TH_FAILED;
		LightLock_Unlock(&lock);
		uploads++;
	}
	if (uploads) return true;
	// Report whether downloads are still pending; the placeholder cards pulse until they land.
	bool pending = false;
	LightLock_Lock(&lock);
	for (int i = 0; i < SLOTS && !pending; i++)
		pending = slots[i].state == TH_QUEUED || slots[i].state == TH_LOADING || slots[i].state == TH_PIXELS;
	LightLock_Unlock(&lock);
	return pending;
}

bool thumbs_get(const char* id, const char* layout, C2D_Image* left, C2D_Image* right, u32* avg_rgb, int* disparity)
{
	if (!id || !*id) return false;
	for (int i = 0; i < SLOTS; i++)
	{
		Slot* s = &slots[i];
		// match the layout too, so a mode change shows the new picture on the card
		if (s->state == TH_READY && !strcmp(s->id, id) && !strcmp(s->layout, layout))
		{
			s->last_used = frame_no;
			*left = s->img[0];
			*right = s->img[1];
			if (avg_rgb) *avg_rgb = s->avg;
			if (disparity) *disparity = s->count == 2 ? s->disparity : 0;
			return true;
		}
	}
	return false;
}
