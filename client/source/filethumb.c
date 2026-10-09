#include "filethumb.h"
#include "app.h"
#include "direct_player.h"
#include "tlog.h"
#include "netfile.h"
#include "files.h"
#include "mem.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_KNOWN        64
#define FNV_OFFSET_BASIS 1469598103934665603ULL
#define FNV_PRIME        1099511628211ULL

typedef struct {
	char id[24];
	char path[256];
	bool have_info;
	u32 duration_ms;
	int w, h;
} Known;

static Known known[MAX_KNOWN];
static int known_next = 0;
static LightLock known_lock = 1;

void filethumb_id(const char* path, char* id, size_t size)
{
	u64 hash = FNV_OFFSET_BASIS;   // FNV-1a
	for (const char* p = path; *p; p++) hash = (hash ^ (u8)*p) * FNV_PRIME;
	snprintf(id, size, "f%016llx", (unsigned long long)hash);
}

static Known* find(const char* id)
{
	for (int i = 0; i < MAX_KNOWN; i++)
		if (known[i].id[0] && !strcmp(known[i].id, id)) return &known[i];
	return NULL;
}

void filethumb_register(const char* id, const char* path)
{
	LightLock_Lock(&known_lock);
	if (!find(id))
	{
		Known* entry = &known[known_next];
		known_next = (known_next + 1) % MAX_KNOWN;
		memset(entry, 0, sizeof(*entry));
		str_copy(entry->id, sizeof(entry->id), id);
		str_copy(entry->path, sizeof(entry->path), path);
	}
	LightLock_Unlock(&known_lock);
}

bool filethumb_info(const char* path, u32* duration_ms, int* w, int* h)
{
	char id[24];
	filethumb_id(path, id, sizeof(id));
	LightLock_Lock(&known_lock);
	Known* entry = find(id);
	bool ok = entry && entry->have_info;
	if (ok)
	{
		*duration_ms = entry->duration_ms;
		*w = entry->w;
		*h = entry->h;
	}
	LightLock_Unlock(&known_lock);
	return ok;
}

// ---------------------------------------------------------------- the file through FFmpeg

// The file being read, either from the SD card (stdio) or from a network place (netfile.c).
typedef struct {
	FILE* f;
	NetFile* nf;
} Src;

static int io_read(void* opaque, uint8_t* buf, int size)
{
	Src* src = (Src*)opaque;
	if (src->nf)
	{
		int got = nf_read(src->nf, buf, size, NULL);
		return got > 0 ? got : got == 0 ? AVERROR_EOF : AVERROR(EIO);
	}
	size_t got = fread(buf, 1, (size_t)size, src->f);
	return got > 0 ? (int)got : AVERROR_EOF;
}

static int64_t io_seek(void* opaque, int64_t off, int whence)
{
	Src* src = (Src*)opaque;
	if (src->nf)
	{
		if (whence == AVSEEK_SIZE) return nf_size(src->nf);
		whence &= ~AVSEEK_FORCE;
		int64_t base = whence == SEEK_CUR ? nf_tell(src->nf) : whence == SEEK_END ? nf_size(src->nf) : 0;
		return nf_seek(src->nf, base + off);
	}
	FILE* f = src->f;
	if (whence == AVSEEK_SIZE)
	{
		off_t saved_pos = ftello(f);
		fseeko(f, 0, SEEK_END);
		off_t size = ftello(f);
		fseeko(f, saved_pos, SEEK_SET);
		return (int64_t)size;
	}
	if (fseeko(f, (off_t)off, whence & ~AVSEEK_FORCE) != 0) return -1;
	return (int64_t)ftello(f);
}

// Converts the avcC header into Annex-B SPS and PPS units. *nal_len receives the size of the
// length prefix in front of each NAL unit in the packets.
static int avcc_to_annexb(const u8* avcc, int len, u8* out, int cap, int* nal_len, u8* level)
{
	if (len < 7 || avcc[0] != 1) return 0;
	*level = avcc[3];
	*nal_len = (avcc[4] & 3) + 1;
	int pos = 5, out_len = 0;
	for (int set = 0; set < 2; set++)   // the SPS set, then the PPS set
	{
		if (pos >= len) return 0;
		int count = set == 0 ? (avcc[pos] & 0x1F) : avcc[pos];
		pos++;
		for (int i = 0; i < count; i++)
		{
			if (pos + 2 > len) return 0;
			int unit_len = avcc[pos] << 8 | avcc[pos + 1];
			pos += 2;
			if (pos + unit_len > len || out_len + 3 + unit_len > cap) return 0;
			out[out_len++] = 0;
			out[out_len++] = 0;
			out[out_len++] = 1;
			memcpy(out + out_len, avcc + pos, (size_t)unit_len);
			out_len += unit_len;
			pos += unit_len;
		}
	}
	return out_len;
}

// Rewrites a packet's length-prefixed NAL units as Annex-B, after the prefix_len bytes that
// are already in out.
static int packet_to_annexb(const AVPacket* pkt, int nal_len, u8* out, int prefix_len, int cap)
{
	int out_len = prefix_len, pos = 0;
	while (pos + nal_len <= pkt->size)
	{
		u32 unit_len = 0;
		for (int i = 0; i < nal_len; i++) unit_len = unit_len << 8 | pkt->data[pos + i];
		pos += nal_len;
		// unit_len is unsigned so that a crafted file's length of 2 GB or more can't turn negative.
		if (unit_len == 0 || unit_len > (u32)(pkt->size - pos) || out_len + 3 > cap ||
		    unit_len > (u32)(cap - out_len - 3))
			return 0;
		out[out_len++] = 0;
		out[out_len++] = 0;
		out[out_len++] = 1;
		memcpy(out + out_len, pkt->data + pos, unit_len);
		out_len += (int)unit_len;
		pos += (int)unit_len;
	}
	return out_len;
}

// ---------------------------------------------------------------- one frame through MVD

// MVD doesn't say whether a call produced a picture. So we write marker bytes into the four
// corners of the output buffer first, and a picture has arrived once any of them changes.
#define MARKER 0x11
static void mark(u8* buf, u32 w, u32 bytes)
{
	const u32 corners[4] = { 0, w * 2 - 1, bytes - w * 2, bytes - 1 };
	for (int i = 0; i < 4; i++)
	{
		buf[corners[i]] = MARKER;
		svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)(uintptr_t)(buf + corners[i]), 1);
	}
}

static bool written(u8* buf, u32 w, u32 bytes)
{
	const u32 corners[4] = { 0, w * 2 - 1, bytes - w * 2, bytes - 1 };
	bool any = false;
	for (int i = 0; i < 4; i++)
	{
		svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)(uintptr_t)(buf + corners[i]), 1);
		any = any || buf[corners[i]] != MARKER;
	}
	return any;
}

#define PREVIEW_AUS        12    // access units read for a preview: the keyframe and those after it
#define MAX_RENDER_CALLS   50    // render calls per access unit while MVD says it's busy
#define KEY_SEARCH_PACKETS 400   // packets read while looking for a keyframe
#define THUMB_MAX_W        320   // the preview's width limit in px

// Feeds access units to MVD, keyframe first, until it writes a picture into out (mw x mh,
// BGR565). The units were all read in advance, so nothing touches the file or the network
// while we hold MVD.
static bool decode_picture(AVPacket* const* aus, int n_aus, const u8* sps_pps, int sps_len, int nal_len, u8* nal,
                           int nal_cap, u8* out, u32 mw, u32 mh)
{
	const u32 bytes = mw * mh * 2;
	MVDSTD_Config cfg;
	mvdstdGenerateDefaultConfig(&cfg, mw, mh, mw, mh, NULL, (u32*)out, (u32*)out);
	cfg.physaddr_outdata0 = osConvertVirtToPhys(out);
	mvdstdRenderVideoFrame(&cfg, false);   // libctru applies the configuration through a render call
	// Send the parameter sets on their own, then the keyframe twice, the same way the player
	// starts a session.
	memcpy(nal, sps_pps, (size_t)sps_len);
	mvdstdProcessVideoFrame(nal, (size_t)sps_len, 0, NULL);
	for (int au = 0; au < n_aus; au++)
	{
		int len = packet_to_annexb(aus[au], nal_len, nal, au == 0 ? sps_len : 0, nal_cap);
		if (au == 0) memcpy(nal, sps_pps, (size_t)sps_len);
		if (len <= 0) return false;
		GSPGPU_FlushDataCache(nal, (u32)len);
		mark(out, mw, bytes);
		if (au == 0) mvdstdProcessVideoFrame(nal, (size_t)len, 0, NULL);
		mvdstdProcessVideoFrame(nal, (size_t)len, 0, NULL);
		if (written(out, mw, bytes)) return true;
		for (int attempt = 0; attempt < MAX_RENDER_CALLS; attempt++)
		{
			Result res = mvdstdRenderVideoFrame(&cfg, false);
			if (written(out, mw, bytes)) return true;
			if (res != MVD_STATUS_BUSY) break;
		}
		// No picture yet because of the reordering delay, so feed the next access unit.
	}
	return false;
}

bool filethumb_frame(const char* id, u8** rgb_out, int* ow, int* oh)
{
	char path[256];
	LightLock_Lock(&known_lock);
	Known* entry = find(id);
	if (entry) str_copy(path, sizeof(path), entry->path);
	LightLock_Unlock(&known_lock);
	if (!entry) return false;

	const u64 start_ms = osGetTime();   // each step is timed for the log (which a release build doesn't have)
	u64 t_open = 0, t_key = 0, t_read = 0, t_mvd = 0;
	Src src = { NULL, NULL };
	if (files_is_network(path))
	{
		char err[160];
		src.nf = nf_open(path, 1024 * 1024, err, sizeof(err));   // enough for the headers and a keyframe
		if (!src.nf)
		{
			tlog("filethumb: %s: %s", path, err);
			return false;
		}
	}
	else
	{
		src.f = fopen(path, "rb");
		if (!src.f) return false;
		setvbuf(src.f, NULL, _IOFBF, 64 * 1024);
	}
	const int iob_size = 32 * 1024;
	u8* iobuf = (u8*)av_malloc((size_t)iob_size);
	AVIOContext* avio = iobuf ? avio_alloc_context(iobuf, iob_size, 0, &src, io_read, NULL, io_seek) : NULL;
	AVFormatContext* fmt_ctx = avformat_alloc_context();
	AVPacket* pkt = av_packet_alloc();
	u8* nal = NULL;
	u8* out = NULL;
	AVPacket* aus[PREVIEW_AUS] = { NULL };
	int n_aus = 0;
	bool ok = false, mvd = false;
	if (!avio || !fmt_ctx || !pkt) goto done;
	fmt_ctx->pb = avio;
	fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;
	fmt_ctx->probesize = 1024 * 1024;
	const char* dot = strrchr(path, '.');
	const AVInputFormat* fmt =
	    dot && (!strcasecmp(dot, ".mkv") || !strcasecmp(dot, ".webm") || !strcasecmp(dot, ".mk3d"))
	        ? av_find_input_format("matroska")
	        : av_find_input_format("mov");
	if (avformat_open_input(&fmt_ctx, NULL, fmt, NULL) < 0)
	{
		fmt_ctx = NULL;
		goto done;
	}
	t_open = osGetTime();
	int video_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (video_index < 0) goto done;
	AVStream* video_stream = fmt_ctx->streams[video_index];
	const u32 width = (u32)video_stream->codecpar->width, height = (u32)video_stream->codecpar->height;
	const u32 mw = (width + 15) & ~15u, mh = (height + 15) & ~15u;   // MVD's size: whole 16 px macroblocks
	const u32 duration_ms = fmt_ctx->duration > 0 ? (u32)(fmt_ctx->duration / (AV_TIME_BASE / 1000)) : 0;
	LightLock_Lock(&known_lock);
	if ((entry = find(id)) != NULL)
	{
		entry->have_info = true;
		entry->duration_ms = duration_ms;
		entry->w = (int)width;
		entry->h = (int)height;
	}
	LightLock_Unlock(&known_lock);
	// Previews go up to 1080p. That's safe as long as the work buffer is sized from the H.264
	// level (direct_mvd_workbuf): an undersized buffer crashes the MVD module at 1080p. On the
	// console, 40 previews in a row followed by a 1080p video were fine, at about 0.45 s each.
	if (video_stream->codecpar->codec_id != AV_CODEC_ID_H264 || width < 16 || height < 16 || mw > 1920u || mh > 1088u)
		goto done;

	u8 sps_pps[512], level = 0;
	int nal_len = 4;
	int sps_len = avcc_to_annexb(video_stream->codecpar->extradata, video_stream->codecpar->extradata_size, sps_pps,
	                             sizeof(sps_pps), &nal_len, &level);
	if (sps_len <= 0) goto done;
	// Seek about a tenth of the way in, past black openings and studio logos.
	if (duration_ms > 20000)
	{
		int64_t ts = av_rescale_q((int64_t)duration_ms / 10, (AVRational){ 1, 1000 }, video_stream->time_base);
		avformat_seek_file(fmt_ctx, video_index, INT64_MIN, ts, ts, 0);
	}
	bool found_key = false;
	for (int i = 0; i < KEY_SEARCH_PACKETS && !found_key && av_read_frame(fmt_ctx, pkt) >= 0; i++)
	{
		found_key = pkt->stream_index == video_index && (pkt->flags & AV_PKT_FLAG_KEY);
		if (!found_key) av_packet_unref(pkt);
	}
	if (!found_key) goto done;
	t_key = osGetTime();
	// Read the keyframe and the access units after it now. MVD is shared with playback, so we
	// never hold it while reading from a network place, or pressing Play would have to wait.
	aus[n_aus++] = pkt;
	pkt = NULL;
	while (n_aus < PREVIEW_AUS)
	{
		AVPacket* next = av_packet_alloc();
		if (!next) break;
		bool got = false;
		while (av_read_frame(fmt_ctx, next) >= 0)
		{
			if (next->stream_index == video_index)
			{
				got = true;
				break;
			}
			av_packet_unref(next);
		}
		if (!got)
		{
			av_packet_free(&next);
			break;
		}
		aus[n_aus++] = next;
	}

	t_read = osGetTime();
	const int nal_cap = 1024 * 1024 + 1024;
	nal = (u8*)linearMemAlign((size_t)nal_cap, 0x40);
	out = (u8*)linearMemAlign(mw * mh * 2, 0x80);
	if (!nal || !out) goto done;
	direct_mvd_lock();
	u32 workbuf_size = direct_mvd_workbuf(mw, mh, level);
	tlog("filethumb: %lux%lu level %u: MVD work buffer %lu KB, linear free %lu KB", (unsigned long)mw,
	     (unsigned long)mh, (unsigned)level, (unsigned long)(workbuf_size / 1024),
	     (unsigned long)(mem_linear_free() / 1024));
	Result mvd_res = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_BGR565, workbuf_size, NULL);
	mvd = R_SUCCEEDED(mvd_res);
	bool got_picture = mvd && decode_picture(aus, n_aus, sps_pps, sps_len, nal_len, nal, nal_cap, out, mw, mh);
	if (mvd) mvdstdExit();
	direct_mvd_unlock();
	t_mvd = osGetTime();
	if (!got_picture)
	{
		tlog("filethumb: no picture from %s (MVD 0x%08lX)", path, (unsigned long)mvd_res);
		goto done;
	}
	// Convert MVD's BGR565 into RGB888 at most THUMB_MAX_W wide, keeping only the visible picture.
	svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)(uintptr_t)out, mw * mh * 2);
	const u32 step = (width + THUMB_MAX_W - 1) / THUMB_MAX_W;   // at least 1, since width >= 16
	const int thumb_w = (int)(width / step), thumb_h = (int)(height / step);
	u8* rgb = (u8*)malloc((size_t)thumb_w * (size_t)thumb_h * 3);
	if (!rgb) goto done;
	const u16* pixels = (const u16*)out;
	for (int y = 0; y < thumb_h; y++)
		for (int x = 0; x < thumb_w; x++)
		{
			u16 pixel = pixels[(u32)y * step * mw + (u32)x * step];
			u8* dst = rgb + ((size_t)y * (size_t)thumb_w + (size_t)x) * 3;
			dst[0] = (u8)((pixel >> 11) << 3);
			dst[1] = (u8)(((pixel >> 5) & 63) << 2);
			dst[2] = (u8)((pixel & 31) << 3);
		}
	*rgb_out = rgb;
	*ow = thumb_w;
	*oh = thumb_h;
	ok = true;
	tlog("filethumb: %lux%lu in %llu ms: open %llu, keyframe %llu, read %d AUs %llu, MVD %llu, convert %llu",
	     (unsigned long)width, (unsigned long)height, (unsigned long long)(osGetTime() - start_ms),
	     (unsigned long long)(t_open - start_ms), (unsigned long long)(t_key - t_open), n_aus,
	     (unsigned long long)(t_read - t_key), (unsigned long long)(t_mvd - t_read),
	     (unsigned long long)(osGetTime() - t_mvd));
done:
	if (nal) linearFree(nal);
	if (out) linearFree(out);
	av_packet_free(&pkt);
	for (int i = 0; i < n_aus; i++) av_packet_free(&aus[i]);
	if (fmt_ctx) avformat_close_input(&fmt_ctx);
	if (avio)
	{
		av_freep(&avio->buffer);
		avio_context_free(&avio);
	}
	else av_free(iobuf);
	if (src.f) fclose(src.f);
	nf_close(src.nf);
	return ok;
}
