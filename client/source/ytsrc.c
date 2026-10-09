#include "ytsrc.h"
#include "hstream.h"
#include "livesrc.h"
#include "netfile.h"
#include "subs.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define AVIO_BUF_BYTES   (32 * 1024)
#define VIDEO_NET_BYTES  (512 * 1024)   // HTTP buffer per stream. The packet rings do the real
#define AUDIO_NET_BYTES  (192 * 1024)   // buffering, so these only smooth out the network.
#define MUXED_NET_BYTES  (640 * 1024)
#define LOCAL_READ_BYTES (128 * 1024)        // stdio buffer for files on the SD card
#define NETFILE_WINDOW   (4 * 1024 * 1024)   // read-ahead for SMB / SFTP files, about 4 s of 1080p
#define LIVE_NET_BYTES   (1024 * 1024)       // live chunks downloaded ahead, about 5 s of 480p
#define LIVE_JUMP_MS     8000                // a live timestamp jump this big starts a new run
#define RING_WAIT_NS     (5 * 1000000ULL)    // nap while a full ring waits for the player

typedef struct {
	// exactly one of these four is set
	HStream* hs;   // a network stream over HTTP
	FILE* f;       // a local file on the SD card
	NetFile* nf;   // a file on an SMB share or SFTP server
	LiveSrc* ls;   // a live stream's chunks, one after another
	int64_t fsize;
	AVIOContext* avio;
	AVFormatContext* fc;
	volatile bool* stop;
} Input;

typedef struct {
	Input vin, ain;   // ain.fc is NULL when both streams come muxed from vin
	bool muxed;
	int vidx, aidx;   // stream indexes within their contexts
	AVCodecContext* adec;
	AVFrame* frame;
	AVPacket* vpkt;
	AVPacket* apkt;
	bool have_v, have_a, v_eof, a_eof;
	u8 sps_pps[512];   // Annex-B SPS + PPS, sent before every keyframe
	u32 sps_pps_len;
	int nal_len_size;   // bytes of the big-endian NAL length prefix (1, 2 or 4)
	s16 pcm[YTSRC_AUDIO_FRAMES * 2];
	u32 pcm_frames;
	u32 pcm_pts;
	char* urls[3];
	u32 gen;                       // seek generation of the packets being produced (see YtSrc.gen)
	int audio_want;                // the file's audio stream to play, or -1 for its default
	LightLock sub_lock;            // guards subs: this thread adds cues, the player reads them
	Subs subs[YTSRC_MAX_TRACKS];   // cues of the file's text subtitle tracks (YtSrc.text)
	Thread thread;
	// Live streams only. The picture size, frame rate and H.264 level come from the playlist,
	// because the TS has no avcC (its pictures are already Annex-B). The timestamps are
	// rebased so they run on smoothly.
	bool live;
	int live_w, live_h, live_fps, live_lv;
	bool have_base;
	int64_t base_ms, last_raw_ms;
	AVRational vtb;   // the video stream's time base
} Priv;

// A YouTube live stream, as yt.cpp encodes it:
// "live:w=854,h=480,fps=30000,lv=31;<HLS media playlist URL>"
static inline bool is_live(const char* url) { return url && !strncmp(url, "live:", 5); }

// True when the player asked for a seek this thread hasn't started on yet, which means it
// should stop filling the rings
static inline bool seek_pending(const YtSrc* s, const Priv* p) { return s->seek_req != p->gen; }

// ---------------------------------------------------------------- byte source for FFmpeg

static int io_read(void* opaque, uint8_t* buf, int size)
{
	Input* in = (Input*)opaque;
	if (in->ls)
	{
		int n = ls_read(in->ls, buf, size, in->stop);
		return n > 0 ? n : n == 0 ? AVERROR_EOF : AVERROR(EIO);
	}
	if (in->nf)
	{
		int n = nf_read(in->nf, buf, size, in->stop);
		return n > 0 ? n : n == 0 ? AVERROR_EOF : AVERROR(EIO);
	}
	if (in->f)
	{
		size_t got = fread(buf, 1, (size_t)size, in->f);
		if (got > 0) return (int)got;
		return feof(in->f) ? AVERROR_EOF : AVERROR(EIO);
	}
	int n = hs_read(in->hs, buf, size, in->stop);
	if (n == 0) return AVERROR_EOF;
	if (n < 0) return AVERROR(EIO);
	return n;
}

static int64_t io_seek(void* opaque, int64_t offset, int whence)
{
	Input* in = (Input*)opaque;
	if (in->ls) return -1;   // a live stream only goes forward
	if (in->nf)
	{
		if (whence == AVSEEK_SIZE) return nf_size(in->nf);
		whence &= ~AVSEEK_FORCE;
		int64_t base = whence == SEEK_CUR ? nf_tell(in->nf) : whence == SEEK_END ? nf_size(in->nf) : 0;
		return nf_seek(in->nf, base + offset);
	}
	if (in->f)
	{
		if (whence == AVSEEK_SIZE) return in->fsize;
		if (fseeko(in->f, (off_t)offset, whence & ~AVSEEK_FORCE) != 0) return -1;
		return (int64_t)ftello(in->f);
	}
	if (whence == AVSEEK_SIZE)
	{
		// The size arrives with the first response, so wait for it briefly
		for (int i = 0; i < 500 && hs_size(in->hs) < 0 && !*in->stop && !hs_error(in->hs, NULL, 0); i++)
			svcSleepThread(10 * 1000000ULL);
		return hs_size(in->hs);
	}
	whence &= ~AVSEEK_FORCE;
	int64_t base = whence == SEEK_CUR ? hs_tell(in->hs) : whence == SEEK_END ? hs_size(in->hs) : 0;
	if (whence == SEEK_END && base < 0) return -1;
	return hs_seek(in->hs, base + offset);
}

// Reports an input's download error, if it has one: the message goes into out and the HTTP
// status into *status.
static bool input_error(Input* in, char* out, size_t len, int* status)
{
	if (in->nf) return nf_error(in->nf, out, len);
	if (in->ls)
	{
		if (!ls_error(in->ls, out, len)) return false;
		*status = ls_http_status(in->ls);
		return true;
	}
	if (!in->hs || !hs_error(in->hs, out, len)) return false;   // local files report errors through FFmpeg
	*status = hs_http_status(in->hs);
	return true;
}

// Opens one input for FFmpeg. On failure, err (and *status, see YtSrc.http_status) says why;
// close_input() cleans up either way.
static bool open_input(Input* in, const char* url, u32 net_bytes, volatile bool* stop, char* err, size_t errlen,
                       int* status)
{
	in->stop = stop;
	const bool is_file = ytsrc_is_file(url);   // SD card or network share: the extension picks the demuxer
	const bool live = is_live(url);
	if (live)
	{
		const char* playlist = strchr(url, ';');
		in->ls = playlist ? ls_open(playlist + 1, LIVE_NET_BYTES) : NULL;
		if (!in->ls)
		{
			snprintf(err, errlen, "Not enough memory to open the live stream.");
			return false;
		}
	}
	else if (ytsrc_is_netfile(url))
	{
		in->nf = nf_open(url, NETFILE_WINDOW, err, errlen);
		if (!in->nf) return false;
	}
	else if (is_file)
	{
		in->f = fopen(url, "rb");
		if (!in->f)
		{
			snprintf(err, errlen, "The file couldn't be opened.");
			return false;
		}
		setvbuf(in->f, NULL, _IOFBF, LOCAL_READ_BYTES);   // so the SD card is read in big blocks
		fseeko(in->f, 0, SEEK_END);
		in->fsize = (int64_t)ftello(in->f);
		fseeko(in->f, 0, SEEK_SET);
	}
	else in->hs = hs_open(url, net_bytes);
	u8* iobuf = (u8*)av_malloc(AVIO_BUF_BYTES);
	if ((!is_file && !live && !in->hs) || !iobuf)
	{
		av_free(iobuf);
		snprintf(err, errlen, "Not enough memory to open the video.");
		return false;
	}
	in->avio = avio_alloc_context(iobuf, AVIO_BUF_BYTES, 0, in, io_read, NULL, io_seek);
	if (!in->avio) av_free(iobuf);   // otherwise the AVIOContext owns it and close_input() frees it
	in->fc = avformat_alloc_context();
	if (!in->avio || !in->fc)
	{
		snprintf(err, errlen, "Not enough memory to open the video.");
		return false;
	}
	in->avio->seekable = live ? 0 : AVIO_SEEKABLE_NORMAL;
	in->fc->pb = in->avio;
	// Drop a packet that was cut short (which only happens when playback stops mid-read)
	// rather than hand it to the hardware decoder
	in->fc->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_DISCARD_CORRUPT;
	in->fc->probesize = 32 * 1024;
	// YouTube's streams are always MP4. A file's extension names the container; without a
	// known one, FFmpeg probes.
	const AVInputFormat* fmt = av_find_input_format("mov");
	if (is_file)
	{
		const char* dot = strrchr(url, '.');
		fmt = NULL;
		if (dot && (!strcasecmp(dot, ".mkv") || !strcasecmp(dot, ".webm") || !strcasecmp(dot, ".mk3d")))
			fmt = av_find_input_format("matroska");
		else if (dot && (!strcasecmp(dot, ".mp4") || !strcasecmp(dot, ".m4v") || !strcasecmp(dot, ".mov")))
			fmt = av_find_input_format("mov");
		in->fc->probesize = 1024 * 1024;
	}
	if (live)
	{
		// MPEG-TS. The audio's sample rate and channel count are only known once its first
		// frames are seen, which avformat_find_stream_info() in source_main does within
		// about 1.5 s of the stream.
		fmt = av_find_input_format("mpegts");
		in->fc->probesize = 512 * 1024;
		in->fc->max_analyze_duration = 1500000;
	}
	int rc = avformat_open_input(&in->fc, NULL, fmt, NULL);
	if (rc < 0)
	{
		if (!input_error(in, err, errlen, status))
		{
			if (*stop) snprintf(err, errlen, "Stopped.");
			else snprintf(err, errlen, "Couldn't read the video file (%d).", rc);
		}
		in->fc = NULL;   // avformat_open_input frees it on failure
		return false;
	}
	return true;
}

static void close_input(Input* in)
{
	if (in->fc) avformat_close_input(&in->fc);
	if (in->avio)
	{
		av_freep(&in->avio->buffer);
		avio_context_free(&in->avio);
	}
	if (in->hs) hs_close(in->hs);
	if (in->f) fclose(in->f);
	if (in->nf) nf_close(in->nf);
	if (in->ls) ls_close(in->ls);
	memset(in, 0, sizeof(*in));
}

// ---------------------------------------------------------------- H.264 helpers

static u32 read_be(const u8* bytes, int count)
{
	u32 value = 0;
	for (int i = 0; i < count; i++) value = (value << 8) | bytes[i];
	return value;
}

// Parses an avcC box (ISO 14496-15) into Annex-B SPS/PPS and the NAL length size.
static bool parse_avcc(Priv* p, YtSrc* s, const u8* avcc, int len)
{
	if (len < 7 || avcc[0] != 1) return false;
	s->h264_level = avcc[3];
	p->nal_len_size = (avcc[4] & 3) + 1;
	int pos = 5;
	p->sps_pps_len = 0;
	for (int list = 0; list < 2; list++)
	{
		if (pos >= len) return false;
		int count = list == 0 ? (avcc[pos] & 0x1F) : avcc[pos];
		pos++;
		for (int i = 0; i < count; i++)
		{
			if (pos + 2 > len) return false;
			int nal_size = (int)read_be(avcc + pos, 2);
			pos += 2;
			if (pos + nal_size > len || p->sps_pps_len + 3 + nal_size > sizeof(p->sps_pps)) return false;
			p->sps_pps[p->sps_pps_len++] = 0;
			p->sps_pps[p->sps_pps_len++] = 0;
			p->sps_pps[p->sps_pps_len++] = 1;
			memcpy(p->sps_pps + p->sps_pps_len, avcc + pos, nal_size);
			p->sps_pps_len += nal_size;
			pos += nal_size;
		}
	}
	return p->sps_pps_len > 0;
}

static u32 to_ms(int64_t ts, AVRational tb)
{
	if (ts == AV_NOPTS_VALUE) return 0;
	int64_t ms = av_rescale_q(ts, tb, (AVRational){ 1, 1000 });
	return ms < 0 ? 0 : (u32)ms;
}

// A packet's time in ms. A live stream's timestamps can start anywhere, and they jump at a
// discontinuity, at the 33-bit wrap, or when the source catches up with the live edge. So we
// count them from the first one and take out any jump, keeping time running on. Picture and
// sound share the same base.
static u32 stream_ms(Priv* p, int64_t ts, AVRational tb)
{
	if (!p->live) return to_ms(ts, tb);
	if (ts == AV_NOPTS_VALUE) return 0;
	int64_t ms = av_rescale_q(ts, tb, (AVRational){ 1, 1000 });
	if (!p->have_base)
	{
		p->have_base = true;
		p->base_ms = p->last_raw_ms = ms;
	}
	if (ms - p->last_raw_ms > LIVE_JUMP_MS || p->last_raw_ms - ms > LIVE_JUMP_MS)
		p->base_ms += ms - p->last_raw_ms - 40;   // carry on about one frame after the last one
	p->last_raw_ms = ms;
	ms -= p->base_ms;
	return ms < 0 ? 0 : (u32)ms;
}

// Finds the next NAL unit in an Annex-B buffer, starting at *pos, and returns its start and
// length. Zero bytes just before the next start code count as part of that code. Returns
// false when there are no more units.
static bool next_nal(const u8* data, int size, int* pos, int* at, int* len)
{
	int i = *pos;
	while (i + 3 <= size && !(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)) i++;
	if (i + 3 > size) return false;
	*at = i + 3;
	int j = *at;
	while (j + 3 <= size && !(data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 1)) j++;
	if (j + 3 > size) j = size;
	*pos = j;
	int end = j;
	while (end > *at && data[end - 1] == 0) end--;   // trailing zeros belong to a 4-byte next code (00 00 00 01)
	*len = end - *at;
	return true;
}

// Queues a live stream's access unit, which is already Annex-B, rewritten into the same form
// push_video produces: every NAL unit behind a 3-byte start code and no access unit
// delimiters, so MVD sees exactly what it gets from MP4. It's a keyframe if it has an IDR
// slice, and disposable if all its slices have nal_ref_idc 0.
static bool push_video_annexb(YtSrc* s, Priv* p, AVPacket* pkt)
{
	const u8* data = pkt->data;
	const int size = pkt->size;
	bool key = false, slices = false, referenced = false;
	u32 total = 0;
	int pos = 0, at, len;
	while (next_nal(data, size, &pos, &at, &len))
	{
		if (len <= 0) continue;
		const u8 header = data[at], type = header & 0x1F;
		if (type == 9) continue;   // access unit delimiter
		if (type == 1 || type == 5)
		{
			slices = true;
			referenced = referenced || type == 5 || (header >> 5) != 0;
			key = key || type == 5;
		}
		total += 3 + (u32)len;
	}
	if (!slices || total > YTSRC_MAX_AU) return true;
	bool wraps;
	Pkt* out;
	while ((out = ring_reserve(s->vq, total, &wraps)) == NULL)
	{
		if (s->stop || seek_pending(s, p)) return false;
		svcSleepThread(RING_WAIT_NS);
	}
	u8* dst = out->data;
	pos = 0;
	while (next_nal(data, size, &pos, &at, &len))
	{
		if (len <= 0 || (data[at] & 0x1F) == 9) continue;
		dst[0] = 0;
		dst[1] = 0;
		dst[2] = 1;
		memcpy(dst + 3, data + at, (size_t)len);
		dst += 3 + len;
	}
	out->type = 'V';
	out->eye = (u8)p->gen;
	out->pad = (u8)((key ? YTSRC_KEYFRAME : 0) | (!referenced ? YTSRC_DISPOSABLE : 0));
	out->pts = stream_ms(p, pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts, p->vtb);
	out->data[total] = 0;
	ring_commit(s->vq, out, wraps);
	return true;
}

// Converts one AVCC access unit to Annex-B, writing it straight into a ring packet.
static bool push_video(YtSrc* s, Priv* p, AVPacket* pkt, AVStream* st)
{
	if (p->live) return push_video_annexb(s, p, pkt);
	bool key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
	// First pass: work out the size, check the lengths, and see whether any other picture
	// needs this one. If all its slices have nal_ref_idc 0 (a B frame nothing refers to), the
	// player may skip it when MVD falls behind.
	u32 total = key ? p->sps_pps_len : 0;
	bool slices = false, referenced = false;
	for (int pos = 0; pos + p->nal_len_size <= pkt->size;)
	{
		u32 nal_size = read_be(pkt->data + pos, p->nal_len_size);
		pos += p->nal_len_size;
		if (nal_size == 0 || nal_size > (u32)(pkt->size - pos)) return true;   // corrupt, so skip the packet
		const u8 header = pkt->data[pos], type = header & 0x1F;
		if (type == 1 || type == 5)
		{
			slices = true;
			referenced = referenced || type == 5 || (header >> 5) != 0;
		}
		total += 3 + nal_size;
		pos += nal_size;
	}
	if (total == 0 || total > YTSRC_MAX_AU) return true;

	bool wraps;
	Pkt* out;
	while ((out = ring_reserve(s->vq, total, &wraps)) == NULL)
	{
		if (s->stop || seek_pending(s, p)) return false;
		svcSleepThread(RING_WAIT_NS);
	}
	u8* dst = out->data;
	if (key)
	{
		memcpy(dst, p->sps_pps, p->sps_pps_len);
		dst += p->sps_pps_len;
	}
	for (int pos = 0; pos + p->nal_len_size <= pkt->size;)
	{
		u32 nal_size = read_be(pkt->data + pos, p->nal_len_size);
		pos += p->nal_len_size;
		dst[0] = 0;
		dst[1] = 0;
		dst[2] = 1;
		memcpy(dst + 3, pkt->data + pos, nal_size);
		dst += 3 + nal_size;
		pos += nal_size;
	}
	out->type = 'V';
	out->eye = (u8)p->gen;
	out->pad = (u8)((key ? YTSRC_KEYFRAME : 0) | (slices && !referenced ? YTSRC_DISPOSABLE : 0));
	out->pts = to_ms(pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts, st->time_base);
	out->data[total] = 0;
	ring_commit(s->vq, out, wraps);
	return true;
}

// ---------------------------------------------------------------- audio

static bool flush_pcm(YtSrc* s, Priv* p)
{
	if (!p->pcm_frames) return true;
	u32 bytes = p->pcm_frames * s->audio_ch * 2;
	bool wraps;
	Pkt* out;
	while ((out = ring_reserve(s->aq, bytes, &wraps)) == NULL)
	{
		if (s->stop || seek_pending(s, p)) return false;
		svcSleepThread(RING_WAIT_NS);
	}
	memcpy(out->data, p->pcm, bytes);
	out->type = 'A';
	out->eye = (u8)p->gen;
	out->pad = 0;
	out->pts = p->pcm_pts;
	out->data[bytes] = 0;
	ring_commit(s->aq, out, wraps);
	p->pcm_frames = 0;
	return true;
}

static inline s16 clip16(float v)
{
	int scaled = (int)(v * 32767.0f);
	return scaled > 32767 ? 32767 : scaled < -32768 ? -32768 : (s16)scaled;
}

// Sample idx of channel c in frame f, as a float in -1..1, for any common sample format
static float sample_at(const AVFrame* f, int c, int idx, int in_ch)
{
	switch (f->format)
	{
	case AV_SAMPLE_FMT_FLTP: return ((const float*)f->extended_data[c])[idx];
	case AV_SAMPLE_FMT_FLT:  return ((const float*)f->data[0])[idx * in_ch + c];
	case AV_SAMPLE_FMT_S16P: return ((const s16*)f->extended_data[c])[idx] * (1.0f / 32768.0f);
	case AV_SAMPLE_FMT_S16:  return ((const s16*)f->data[0])[idx * in_ch + c] * (1.0f / 32768.0f);
	case AV_SAMPLE_FMT_S32P: return (float)((const int32_t*)f->extended_data[c])[idx] * (1.0f / 2147483648.0f);
	case AV_SAMPLE_FMT_S32:  return (float)((const int32_t*)f->data[0])[idx * in_ch + c] * (1.0f / 2147483648.0f);
	default:                 return 0.0f;
	}
}

// Stereo weights for each input channel of a surround layout such as 5.1 or 7.1. Front
// channels go to their own side. The centre (dialogue) goes into both sides at -3 dB, and the
// surrounds into their own side at -3 dB. The LFE channel is left out. Everything is scaled
// so a full-scale front plus centre doesn't clip.
static void downmix_weights(const AVChannelLayout* layout, float* wl, float* wr)
{
	const float minus_3db = 0.7071f;
	for (int c = 0; c < layout->nb_channels; c++)
	{
		enum AVChannel ch = av_channel_layout_channel_from_index(layout, (unsigned)c);
		float left = 0, right = 0;
		switch (ch)
		{
		case AV_CHAN_FRONT_LEFT:
		case AV_CHAN_FRONT_LEFT_OF_CENTER:
		case AV_CHAN_WIDE_LEFT:             left = 1; break;
		case AV_CHAN_FRONT_RIGHT:
		case AV_CHAN_FRONT_RIGHT_OF_CENTER:
		case AV_CHAN_WIDE_RIGHT:            right = 1; break;
		case AV_CHAN_FRONT_CENTER:          left = right = minus_3db; break;
		case AV_CHAN_SIDE_LEFT:
		case AV_CHAN_BACK_LEFT:
		case AV_CHAN_SURROUND_DIRECT_LEFT:  left = minus_3db; break;
		case AV_CHAN_SIDE_RIGHT:
		case AV_CHAN_BACK_RIGHT:
		case AV_CHAN_SURROUND_DIRECT_RIGHT: right = minus_3db; break;
		case AV_CHAN_LOW_FREQUENCY:
		case AV_CHAN_LOW_FREQUENCY_2:       break;
		default:                            left = right = minus_3db * minus_3db; break;   // back centre, height channels and the rest
		}
		wl[c] = left / (1.0f + minus_3db);
		wr[c] = right / (1.0f + minus_3db);
	}
}

// Converts a decoded frame to interleaved s16 and queues it. Any common sample format works,
// and more than two channels are mixed down to stereo.
static bool push_audio_frame(YtSrc* s, Priv* p, AVFrame* f, AVStream* st)
{
	int in_ch = f->ch_layout.nb_channels;
	int out_ch = s->audio_ch;
	float wl[16], wr[16];
	const bool mix = in_ch > 2 && in_ch <= 16 && out_ch == 2;
	if (mix) downmix_weights(&f->ch_layout, wl, wr);
	u32 pts =
	    stream_ms(p, f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts, st->time_base);
	for (int i = 0; i < f->nb_samples;)
	{
		if (p->pcm_frames == 0) p->pcm_pts = pts + (u32)((u64)i * 1000 / s->audio_rate);
		int count = f->nb_samples - i;
		if ((u32)count > YTSRC_AUDIO_FRAMES - p->pcm_frames) count = YTSRC_AUDIO_FRAMES - p->pcm_frames;
		s16* dst = p->pcm + p->pcm_frames * out_ch;
		for (int k = 0; k < count; k++)
		{
			int idx = i + k;
			if (mix)
			{
				float left = 0, right = 0;
				for (int c = 0; c < in_ch; c++)
				{
					float sample = sample_at(f, c, idx, in_ch);
					left += sample * wl[c];
					right += sample * wr[c];
				}
				*dst++ = clip16(left);
				*dst++ = clip16(right);
			}
			else
				for (int c = 0; c < out_ch; c++) *dst++ = clip16(sample_at(f, c < in_ch ? c : 0, idx, in_ch));
		}
		p->pcm_frames += count;
		i += count;
		if (p->pcm_frames == YTSRC_AUDIO_FRAMES && !flush_pcm(s, p)) return false;
	}
	return true;
}

static bool decode_audio(YtSrc* s, Priv* p, AVPacket* pkt, AVStream* st)
{
	if (avcodec_send_packet(p->adec, pkt) < 0) return true;   // skip a bad packet
	while (avcodec_receive_frame(p->adec, p->frame) == 0)
	{
		bool ok = push_audio_frame(s, p, p->frame, st);
		av_frame_unref(p->frame);
		if (!ok) return false;
	}
	return true;
}

// ---------------------------------------------------------------- thread

// Publishes an error (for http_status, see YtSrc). Only the demux thread calls this.
static void fail(YtSrc* s, const char* msg, int http_status)
{
	if (!s->failed)
	{
		snprintf(s->errmsg, sizeof(s->errmsg), "%s", msg);
		s->http_status = http_status;
		ytsrc_barrier();   // make errmsg and http_status visible before the flag
		s->failed = true;
	}
}

// Fails with the download error of either input, or with fallback if there is none.
static void fail_input(YtSrc* s, Priv* p, const char* fallback)
{
	char err[200];
	int status = 0;
	if (input_error(&p->vin, err, sizeof(err), &status) || input_error(&p->ain, err, sizeof(err), &status))
		fail(s, err, status);
	else fail(s, fallback, 0);
}

// Reads the next packet of the wanted stream from a context. Returns false at the end.
static bool next_packet(Input* in, int want_idx, AVPacket* pkt)
{
	while (av_read_frame(in->fc, pkt) >= 0)
	{
		if (pkt->stream_index == want_idx) return true;
		av_packet_unref(pkt);
	}
	return false;
}

// Moves both inputs to the keyframe at or before ms: the video first, then the audio to that
// keyframe's time. Reads the first video packet into p->vpkt. Set initial when the inputs
// were just opened, so position 0 needs no seek. Returns 1 once positioned, 0 when ms is past
// the last keyframe and nothing is left to play, and -1 after fail() or a stop.
static int seek_to(YtSrc* s, Priv* p, AVStream* vst, AVStream* ast, Input* ain, u32 ms, bool initial)
{
	if (p->have_v) av_packet_unref(p->vpkt);
	if (p->have_a) av_packet_unref(p->apkt);
	p->have_v = p->have_a = p->v_eof = p->a_eof = false;
	p->pcm_frames = 0;
	if (p->adec) avcodec_flush_buffers(p->adec);
	if (ms > 0 || !initial)
	{
		int64_t ts = av_rescale_q((int64_t)ms, (AVRational){ 1, 1000 }, vst->time_base);
		avformat_seek_file(p->vin.fc, p->vidx, INT64_MIN, ts, ts, 0);
	}
	p->have_v = next_packet(&p->vin, p->vidx, p->vpkt);
	if (!p->have_v)
	{
		int status = 0;
		if (s->stop) fail(s, "Stopped.", 0);
		else if (initial || input_error(&p->vin, NULL, 0, &status)) fail_input(s, p, "The video ended.");
		else
		{
			p->v_eof = p->a_eof = true;
			s->start_ms = ms;
			return 0;
		}
		return -1;
	}
	u32 first_ms = stream_ms(p, p->vpkt->pts != AV_NOPTS_VALUE ? p->vpkt->pts : p->vpkt->dts, vst->time_base);
	s->start_ms = first_ms;
	if (ast && !p->muxed && (first_ms > 0 || !initial))
	{
		int64_t ts = av_rescale_q((int64_t)first_ms, (AVRational){ 1, 1000 }, ast->time_base);
		avformat_seek_file(ain->fc, p->aidx, INT64_MIN, ts, ts, 0);
	}
	return 1;
}

// Lists a file's decodable audio tracks and its text subtitle tracks in YtSrc.audio and
// YtSrc.text. Picks the requested audio track (p->audio_want) and tells FFmpeg to discard
// every stream nobody uses.
static void list_tracks(YtSrc* s, Priv* p)
{
	AVFormatContext* fc = p->vin.fc;
	s->audio_cur = -1;
	for (unsigned i = 0; i < fc->nb_streams; i++)
	{
		AVStream* st = fc->streams[i];
		const AVCodecParameters* par = st->codecpar;
		const enum AVCodecID id = par->codec_id;
		const bool audio = par->codec_type == AVMEDIA_TYPE_AUDIO && avcodec_find_decoder(id) != NULL;
		const bool text = par->codec_type == AVMEDIA_TYPE_SUBTITLE &&
		                  (id == AV_CODEC_ID_SUBRIP || id == AV_CODEC_ID_TEXT || id == AV_CODEC_ID_ASS ||
		                   id == AV_CODEC_ID_SSA || id == AV_CODEC_ID_MOV_TEXT || id == AV_CODEC_ID_WEBVTT);
		YtSrcTrack* track = audio && s->n_audio < YTSRC_MAX_TRACKS ? &s->audio[s->n_audio]
		                    : text && s->n_text < YTSRC_MAX_TRACKS ? &s->text[s->n_text]
		                                                           : NULL;
		if (!track)
		{
			if ((int)i != p->vidx) st->discard = AVDISCARD_ALL;   // e.g. a DTS-HD track, PGS, fonts
			continue;
		}
		memset(track, 0, sizeof(*track));
		track->stream = (int)i;
		const AVDictionaryEntry* entry = av_dict_get(st->metadata, "language", NULL, 0);
		if (entry && strcmp(entry->value, "und") != 0) snprintf(track->lang, sizeof(track->lang), "%s", entry->value);
		entry = av_dict_get(st->metadata, "title", NULL, 0);
		if (entry) snprintf(track->title, sizeof(track->title), "%s", entry->value);
		snprintf(track->codec, sizeof(track->codec), "%s", avcodec_get_name(id));
		track->channels = (u8)par->ch_layout.nb_channels;
		track->ass = id == AV_CODEC_ID_ASS || id == AV_CODEC_ID_SSA;
		track->forced = (st->disposition & AV_DISPOSITION_FORCED) != 0;
		if (audio)
		{
			if ((int)i == p->audio_want) p->aidx = (int)i;
			s->n_audio++;
		}
		else s->n_text++;
	}
	for (int a = 0; a < s->n_audio; a++)
		if (s->audio[a].stream == p->aidx) s->audio_cur = a;
	if (s->audio_cur < 0 && s->n_audio)
	{
		// We can't decode the file's default track (TrueHD, say), so use the first one we can
		s->audio_cur = 0;
		p->aidx = s->audio[0].stream;
	}
	for (int a = 0; a < s->n_audio; a++)
		if (a != s->audio_cur) fc->streams[s->audio[a].stream]->discard = AVDISCARD_ALL;   // only one is decoded
}

// Adds the cue in a text subtitle packet to its track's list
static void take_sub_packet(YtSrc* s, Priv* p, const AVPacket* pkt)
{
	for (int track = 0; track < s->n_text; track++)
	{
		if (s->text[track].stream != pkt->stream_index) continue;
		const AVStream* st = p->vin.fc->streams[pkt->stream_index];
		if (pkt->pts == AV_NOPTS_VALUE || !pkt->data || pkt->size <= 0) return;
		const char* text = (const char*)pkt->data;
		size_t len = (size_t)pkt->size;
		if (st->codecpar->codec_id == AV_CODEC_ID_MOV_TEXT)
		{
			// A 16-bit length, then the text, then optional style boxes
			const size_t text_len = len >= 2 ? (size_t)((u8)text[0] << 8 | (u8)text[1]) : 0;
			text += 2;
			len = text_len <= len - 2 ? text_len : 0;
		}
		while (len && text[len - 1] == 0) len--;
		const u32 start = to_ms(pkt->pts, st->time_base);
		const u32 end = pkt->duration > 0 ? start + to_ms(pkt->duration, st->time_base) : 0;
		LightLock_Lock(&p->sub_lock);
		subs_add(&p->subs[track], start, end, text, len, s->text[track].ass);
		LightLock_Unlock(&p->sub_lock);
		return;
	}
}

static void source_main(void* arg)
{
	YtSrc* s = (YtSrc*)arg;
	Priv* p = (Priv*)s->priv;
	char err[200];
	int status = 0;

	p->muxed = !(p->urls[0] && p->urls[1]);
	p->live = p->muxed && is_live(p->urls[2]);
	if (p->live)
	{
		p->live_fps = 30000;
		p->live_lv = 31;
		// Parse "live:w=854,h=480,fps=30000,lv=31;<url>" as built by yt.cpp
		const char* field = p->urls[2] + 5;
		for (int i = 0; i < 4 && *field && *field != ';'; i++)
		{
			const char* eq = strchr(field, '=');
			if (!eq) break;
			const long value = strtol(eq + 1, NULL, 10);
			if (!strncmp(field, "w=", 2)) p->live_w = (int)value;
			else if (!strncmp(field, "h=", 2)) p->live_h = (int)value;
			else if (!strncmp(field, "fps=", 4) && value > 0) p->live_fps = (int)value;
			else if (!strncmp(field, "lv=", 3) && value > 0) p->live_lv = (int)value;
			field = eq + 1 + strcspn(eq + 1, ",;");
			if (*field == ',') field++;
		}
	}
	if (!open_input(&p->vin, p->muxed ? p->urls[2] : p->urls[0], p->muxed ? MUXED_NET_BYTES : VIDEO_NET_BYTES, &s->stop,
	                err, sizeof(err), &status))
	{
		fail(s, err, status);
		return;
	}
	s->net_core = p->vin.hs ? hs_core(p->vin.hs) : -1;
	if (!p->muxed && !open_input(&p->ain, p->urls[1], AUDIO_NET_BYTES, &s->stop, err, sizeof(err), &status))
	{
		fail(s, err, status);
		return;
	}
	Input* ain = p->muxed ? &p->vin : &p->ain;

	if (p->live) avformat_find_stream_info(p->vin.fc, NULL);   // learns the audio's rate and channels
	p->vidx = av_find_best_stream(p->vin.fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	p->aidx = av_find_best_stream(ain->fc, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (p->vidx < 0)
	{
		fail(s, "The video has no picture track.", 0);
		return;
	}
	if (p->muxed) list_tracks(s, p);
	AVStream* vst = p->vin.fc->streams[p->vidx];
	AVStream* ast = p->aidx >= 0 ? ain->fc->streams[p->aidx] : NULL;
	AVCodecParameters* vpar = vst->codecpar;
	p->vtb = vst->time_base;
	if (p->live)
	{
		// The TS carries Annex-B pictures with their own SPS/PPS. The size and level come from
		// the playlist, via yt.cpp.
		if (vpar->codec_id != AV_CODEC_ID_H264 || p->live_w <= 0 || p->live_h <= 0)
		{
			fail(s, "The live stream isn't in a format the 3DS decoder supports (H.264).", 0);
			return;
		}
		s->h264_level = (u8)p->live_lv;
		s->crop_w = p->live_w;
		s->crop_h = p->live_h;
		s->width = (p->live_w + 15) & ~15u;
		s->height = (p->live_h + 15) & ~15u;
		s->fps_x1000 = (u32)p->live_fps;
	}
	else
	{
		if (vpar->codec_id != AV_CODEC_ID_H264 || !parse_avcc(p, s, vpar->extradata, vpar->extradata_size))
		{
			fail(s, "The video isn't in a format the 3DS decoder supports (H.264).", 0);
			return;
		}
		s->crop_w = vpar->width;
		s->crop_h = vpar->height;
		s->width = (vpar->width + 15) & ~15u;
		s->height = (vpar->height + 15) & ~15u;
		AVRational rate = vst->avg_frame_rate.num ? vst->avg_frame_rate : vst->r_frame_rate;
		s->fps_x1000 = rate.num && rate.den ? (u32)((u64)rate.num * 1000 / rate.den) : 30000;
	}
	if (p->live) s->duration_ms = 0;
	else if (p->vin.fc->duration > 0) s->duration_ms = (u32)(p->vin.fc->duration / (AV_TIME_BASE / 1000));
	else if (vst->duration > 0) s->duration_ms = to_ms(vst->duration, vst->time_base);

	if (ast)
	{
		const AVCodec* codec = avcodec_find_decoder(ast->codecpar->codec_id);
		p->adec = codec ? avcodec_alloc_context3(codec) : NULL;
		if (p->adec && avcodec_parameters_to_context(p->adec, ast->codecpar) >= 0 &&
		    avcodec_open2(p->adec, codec, NULL) >= 0)
		{
			s->audio_rate = p->adec->sample_rate ? p->adec->sample_rate : ast->codecpar->sample_rate;
			int ch = p->adec->ch_layout.nb_channels;
			s->audio_ch = ch >= 2 ? 2 : ch == 1 ? 1 : 0;
		}
		if (!s->audio_ch || !s->audio_rate)
		{
			avcodec_free_context(&p->adec);
			s->audio_ch = 0;
			ast = NULL;
		}
	}
	p->frame = av_frame_alloc();
	p->vpkt = av_packet_alloc();
	p->apkt = av_packet_alloc();
	if (!p->frame || !p->vpkt || !p->apkt)
	{
		fail(s, "Not enough memory to play the video.", 0);
		return;
	}

	// A seek requested before the stream was ready simply becomes the starting point
	p->gen = s->seek_req;
	ytsrc_barrier();   // pairs with ytsrc_seek(), which writes seek_ms before seek_req
	if (p->gen) s->start_ms = s->seek_ms;
	if (seek_to(s, p, vst, ast, ain, s->start_ms, true) < 0) return;
	ytsrc_barrier();   // make the stream info visible before the flags
	s->gen = p->gen;
	s->ready = true;

	// The demux loop. It always hands over whichever stream is furthest behind, so the two
	// rings fill at the same pace.
	while (!s->stop)
	{
		if (seek_pending(s, p))
		{
			// An in-place seek keeps the open connections and the parsed index, so it costs
			// one ranged request per stream instead of a whole new start
			p->gen = s->seek_req;
			ytsrc_barrier();
			s->eof = false;
			int result = seek_to(s, p, vst, ast, ain, s->seek_ms, false);
			if (result < 0) break;
			ytsrc_barrier();   // make start_ms visible before gen
			s->gen = p->gen;
			if (result == 0) s->eof = true;   // past the last keyframe, so nothing until the next seek
			continue;
		}
		if (s->eof)
		{
			// The rest of the video is already in the rings; wait for a seek or a stop
			svcSleepThread(20 * 1000000ULL);
			continue;
		}
		if (ast && !p->muxed && !p->have_a && !p->a_eof)
		{
			p->have_a = next_packet(ain, p->aidx, p->apkt);
			p->a_eof = !p->have_a;
		}
		if (!p->have_v && !p->v_eof)
		{
			if (p->muxed)
			{
				// A single context, so take whatever packet comes next
				if (av_read_frame(p->vin.fc, p->vpkt) < 0)
				{
					p->v_eof = true;
					continue;
				}
				if (ast && p->vpkt->stream_index == p->aidx)
				{
					bool ok = decode_audio(s, p, p->vpkt, ast);
					av_packet_unref(p->vpkt);
					if (!ok && !seek_pending(s, p)) break;
					continue;
				}
				if (p->vpkt->stream_index != p->vidx)
				{
					take_sub_packet(s, p, p->vpkt);
					av_packet_unref(p->vpkt);
					continue;
				}
				p->have_v = true;
			}
			else
			{
				p->have_v = next_packet(&p->vin, p->vidx, p->vpkt);
				p->v_eof = !p->have_v;
			}
		}
		if (!p->have_v && (p->a_eof || !ast || p->muxed) && p->v_eof)
		{
			// The end. Publish it, then wait (in the branch above) for a seek back or a stop.
			if (ast && !flush_pcm(s, p))
			{
				if (seek_pending(s, p)) continue;
				break;
			}
			int unused_status = 0;
			if (input_error(&p->vin, NULL, 0, &unused_status) || input_error(&p->ain, NULL, 0, &unused_status))
			{
				fail_input(s, p, "Network error.");
				break;
			}
			ytsrc_barrier();   // make the last ring commits visible before the flag
			s->eof = true;
			continue;
		}

		u32 video_ms = p->have_v
		                   ? stream_ms(p, p->vpkt->pts != AV_NOPTS_VALUE ? p->vpkt->pts : p->vpkt->dts, vst->time_base)
		                   : 0xFFFFFFFF;
		u32 audio_ms = p->have_a && ast ? stream_ms(p, p->apkt->pts, ast->time_base) : 0xFFFFFFFF;
		if (p->have_a && ast && audio_ms <= video_ms)
		{
			bool ok = decode_audio(s, p, p->apkt, ast);
			av_packet_unref(p->apkt);
			p->have_a = false;
			if (!ok && !seek_pending(s, p)) break;
		}
		else if (p->have_v)
		{
			bool ok = push_video(s, p, p->vpkt, vst);
			av_packet_unref(p->vpkt);
			p->have_v = false;
			if (!ok && !seek_pending(s, p)) break;
		}
	}
}

// ---------------------------------------------------------------- public

bool ytsrc_start(YtSrc* s, Ring* vq, Ring* aq, const char* video_url, const char* audio_url, const char* muxed_url,
                 u32 start_ms, int audio_stream)
{
	memset(s, 0, sizeof(*s));
	s->audio_cur = -1;
#ifdef __3DS__
	av_log_set_level(AV_LOG_QUIET);   // nobody reads stderr on the console, so skip the formatting
#endif
	Priv* p = (Priv*)calloc(1, sizeof(Priv));
	if (!p) return false;
	s->priv = p;
	s->vq = vq;
	s->aq = aq;
	s->start_ms = start_ms;
	p->audio_want = audio_stream;
	LightLock_Init(&p->sub_lock);
	p->urls[0] = video_url ? strdup(video_url) : NULL;
	p->urls[1] = audio_url ? strdup(audio_url) : NULL;
	p->urls[2] = muxed_url ? strdup(muxed_url) : NULL;
	if (!(p->urls[0] && p->urls[1]) && !p->urls[2])
	{
		ytsrc_stop(s);
		return false;
	}
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// FFmpeg's mov demuxer and AAC decoder need a big stack. On the extra core this thread may
	// outrank the playback loop; sharing the main core, it must rank below it (see hstream.c).
	s->source_core = 2;
	p->thread = threadCreate(source_main, s, 64 * 1024, prio - 1, 2, false);
	if (!p->thread)
	{
		s->source_core = -1;
		p->thread = threadCreate(source_main, s, 64 * 1024, prio + 2, -1, false);
	}
	if (!p->thread)
	{
		ytsrc_stop(s);
		return false;
	}
	return true;
}

void ytsrc_stop(YtSrc* s)
{
	Priv* p = (Priv*)s->priv;
	if (!p) return;
	s->stop = true;
	if (p->thread)
	{
		threadJoin(p->thread, U64_MAX);
		threadFree(p->thread);
	}
	av_packet_free(&p->vpkt);
	av_packet_free(&p->apkt);
	av_frame_free(&p->frame);
	avcodec_free_context(&p->adec);
	close_input(&p->ain);
	close_input(&p->vin);
	for (int i = 0; i < 3; i++) free(p->urls[i]);
	for (int i = 0; i < YTSRC_MAX_TRACKS; i++) subs_free(&p->subs[i]);
	free(p);
	s->priv = NULL;
}

const char* ytsrc_sub_text(YtSrc* s, int t, u32 ms, char* out, size_t cap)
{
	Priv* p = (Priv*)s->priv;
	if (cap) out[0] = 0;
	if (!p || t < 0 || t >= s->n_text) return out;
	LightLock_Lock(&p->sub_lock);
	subs_at(&p->subs[t], ms, out, cap);
	LightLock_Unlock(&p->sub_lock);
	return out;
}
