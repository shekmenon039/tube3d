#pragma once
// The stream source. It downloads a YouTube video's DASH streams (or the muxed fallback), or
// opens a local, network or live source, demuxes it with FFmpeg and fills the player's two
// packet rings:
//   video ring: one packet per H.264 access unit, already in Annex-B form (00 00 01 + NAL)
//               with SPS/PPS in front of every keyframe, so MVD can take it as is
//   audio ring: 16-bit interleaved PCM, YTSRC_AUDIO_FRAMES frames per packet
// All the work happens on a background thread, usually on another core. The stream info is
// filled in before `ready` is set, and errmsg/http_status before `failed`. Read the flags
// through ytsrc_is_ready(), ytsrc_has_failed() and ytsrc_is_eof(): they include the memory
// barrier that makes the data published with each flag visible on the reading core.
#include <3ds.h>
#include <stdbool.h>
#include <string.h>
#include "ring.h"

#define YTSRC_AUDIO_FRAMES 4096   // PCM frames per audio packet (about 93 ms at 44.1 kHz)

// The tracks of a file, such as an MKV or MP4 with several languages: the audio tracks we can
// decode, and the text subtitle tracks (SubRip, ASS/SSA, mov_text, WebVTT). Picture-based
// subtitles like PGS aren't offered.
#define YTSRC_MAX_TRACKS 12
typedef struct {
	int stream;       // the stream's index in the file
	char lang[8];     // as the file states it, e.g. "eng", or "" if it doesn't
	char title[48];   // as the file states it, e.g. "Commentary", "SDH"
	char codec[12];   // e.g. "ac3", "aac"
	u8 channels;      // audio tracks only
	bool ass;         // subtitle tracks: the cues are ASS/SSA events
	bool forced;      // subtitle tracks: only the foreign-language parts
} YtSrcTrack;

// The largest video packet (one Annex-B access unit including SPS/PPS) that goes into the
// video ring; bigger ones are dropped. The player's copy buffer must hold at least this much.
#define YTSRC_MAX_AU (1024 * 1024)   // keyframes of 1080p movies reach about 600 KB

// A path on the SD card ("sdmc:/..."), played from the file rather than over the network
static inline bool ytsrc_is_local(const char* url) { return url && (url[0] == '/' || !strncmp(url, "sdmc:", 5)); }

// A file on an SMB share or SFTP server (netfile.c, servers.h)
static inline bool ytsrc_is_netfile(const char* url)
{
	return url && (!strncmp(url, "smb://", 6) || !strncmp(url, "sftp://", 7));
}

// A file from the Videos tab, on the SD card or a network place rather than YouTube
static inline bool ytsrc_is_file(const char* url) { return ytsrc_is_local(url) || ytsrc_is_netfile(url); }

typedef struct {
	volatile bool stop;
	volatile bool ready;   // the stream info below is valid
	volatile bool eof;
	volatile bool failed;
	char errmsg[200];
	int http_status;   // when failed: 403/404/410 if the server refused a stream link
	                   // because it expired or was cut off (fetch fresh URLs with
	                   // yt_get_streams or fall back to the muxed stream); otherwise 0

	// stream info
	u32 width, height;    // coded size rounded up to 16, which is what MVD decodes
	u32 crop_w, crop_h;   // visible picture size
	u32 fps_x1000;
	u32 duration_ms;   // from the container, 0 if unknown; YouTube reports its own
	u32 audio_rate;
	u8 audio_ch;       // 0 means no audio
	u8 h264_level;     // from avcC, e.g. 30 for level 3.0
	u32 start_ms;      // pts of the first frame after a seek
	int source_core;   // core of the demux thread: 2 (the New 3DS's extra core) or -1
	int net_core;      // core of the video download thread, valid once ready
	// A file's tracks, valid once ready. YouTube's DASH streams have none.
	YtSrcTrack audio[YTSRC_MAX_TRACKS];
	int n_audio;
	int audio_cur;   // index of the playing track in audio, or -1
	YtSrcTrack text[YTSRC_MAX_TRACKS];
	int n_text;   // their cues arrive along with the packets; see ytsrc_sub_text()

	// In-place seeks (ytsrc_seek). The player bumps seek_req; the thread seeks inside the
	// streams it already has open, without reconnecting or downloading a new index, sets
	// start_ms and publishes gen = seek_req. Every packet carries the low 8 bits of the
	// generation that produced it in Pkt.eye, so packets from before a seek can be recognised
	// and dropped.
	volatile u32 seek_req;
	volatile u32 seek_ms;
	volatile u32 gen;

	Ring* vq;
	Ring* aq;
	void* priv;
} YtSrc;

// Starts the source thread. Give either video_url and audio_url (DASH) or muxed_url.
// audio_stream picks a file's audio track (YtSrcTrack.stream), or -1 for the file's default.
bool ytsrc_start(YtSrc* s, Ring* vq, Ring* aq, const char* video_url, const char* audio_url, const char* muxed_url,
                 u32 start_ms, int audio_stream);

// Like subs_at(), the text of embedded subtitle track t (an index into text) to show at ms.
// Only cues demuxed so far are known, but the demuxer runs seconds ahead of playback.
const char* ytsrc_sub_text(YtSrc* s, int t, u32 ms, char* out, size_t cap);

// Stops the thread and frees everything.
void ytsrc_stop(YtSrc* s);

// Moves the stream to the keyframe at or before ms without reconnecting. Packets already in
// the rings, and a few still on their way, belong to the old generation, so drop every packet
// whose eye != ytsrc_gen_tag(s). The seek clears eof, but a failed source stays failed (start
// a new one). It's fine to call again before the last seek finished; only the newest counts.
static inline void ytsrc_seek(YtSrc* s, u32 ms)
{
	s->seek_ms = ms;
	__sync_synchronize();   // publish the target before the request
	s->seek_req = s->seek_req + 1;
}

// The generation tag carried by packets of the newest requested seek
static inline u8 ytsrc_gen_tag(const YtSrc* s) { return (u8)s->seek_req; }

// Flags stored in Pkt.pad for video packets
#define YTSRC_KEYFRAME   1
#define YTSRC_DISPOSABLE 2   // no other picture refers to this one, so it may be skipped

// Full memory barrier between the source thread's core and the reader's
static inline void ytsrc_barrier(void)
{
#ifdef __3DS__
	__dmb();
#else
	__sync_synchronize();
#endif
}

// Flag reads for the player. The barrier after the read makes whatever was written before
// the flag (the stream info for ready, errmsg/http_status for failed) safe to read.
static inline bool ytsrc_is_ready(const YtSrc* s)
{
	bool v = s->ready;
	ytsrc_barrier();
	return v;
}
static inline bool ytsrc_has_failed(const YtSrc* s)
{
	bool v = s->failed;
	ytsrc_barrier();
	return v;
}
// True once the newest seek has been carried out, with start_ms holding the new position.
// Also true when no seek was ever requested.
static inline bool ytsrc_seek_done(const YtSrc* s)
{
	bool v = s->gen == s->seek_req;
	ytsrc_barrier();
	return v;
}
// End of stream for the current generation; false while a seek is still in progress
static inline bool ytsrc_is_eof(const YtSrc* s)
{
	bool v = s->eof && s->gen == s->seek_req;
	ytsrc_barrier();
	return v;
}
