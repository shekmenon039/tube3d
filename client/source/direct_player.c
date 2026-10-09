// The video player (interface in direct_player.h).
//
// For each frame, the source thread (ytsrc.c) has already put one Annex-B access unit into
// the video ring. MVD decodes it into a linear BGR565 buffer at the coded size, the same way
// FourthTube does. Two GPU copies then get it into a texture without the CPU touching the
// pixels: DisplayTransfer converts it to the GPU's tiled layout, and TextureCopy places those
// tile rows into a power-of-two texture in VRAM, which ui.c draws.
// Audio arrives as decoded PCM in the audio ring and is played by NDSP. It's the master clock.
#include "direct_player.h"
#include "app.h"
#include "avsync.h"
#include "exitguard.h"
#include "mem.h"
#include "playerui.h"
#include "power.h"
#include "ring.h"
#include "stereo.h"
#include "tlog.h"
#include "yt.h"
#include "ytsrc.h"
#include "subs.h"
#include "motion.h"
#include "vmesh.h"
#include "netfile.h"
#include "resume.h"

#include <citro3d.h>
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char g_video_id[16];   // the video playing, or "file" (for comments and votes)

#define THREAD_STACK (64 * 1024)   // stack size of the network threads (comments, lookup, subtitles)

// Background downloads during playback (subtitles, comments, like counts) run one step below the
// decoder worker, which itself runs one step below the playback loop. The 3DS doesn't share time
// between threads of equal priority: at the decoder's level, a TLS handshake (a second of pure
// CPU on a fresh connection) kept the decoder from running and froze the picture at the start.
static s32 background_prio(void)
{
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);   // the playback loop's priority
	return prio + 2 <= 0x3F ? prio + 2 : 0x3F;
}

// ---- the video's comments (YouTube only)
// They're fetched on a thread the first time the panel opens, then a page at a time as it
// scrolls. The video keeps playing meanwhile.
#define MAX_COMMENTS 120
static YtComment g_cm[MAX_COMMENTS];
static volatile int g_cm_n;             // comments in g_cm (the thread fills them in, then bumps this)
static YtCommentsCursor* g_cm_cursor;   // the next page (owned by the thread while it runs)
static Thread g_cm_thread;
static volatile bool g_cm_busy;   // the thread is fetching
static bool g_cm_first;           // ... and it's the first page
static bool g_cm_started, g_cm_end, g_cm_open;
static char g_cm_err[128];
static char g_channel[64], g_views[24];   // shown under the title

void direct_set_meta(const char* channel, const char* views)
{
	str_copy(g_channel, sizeof(g_channel), channel ? channel : "");
	str_copy(g_views, sizeof(g_views), views ? views : "");
}

static void comments_main(void* arg)
{
	(void)arg;
	char err[128] = "";
	YtCommentsCursor* cursor = g_cm_cursor;
	const int have = g_cm_n;
	int got = g_cm_first ? yt_comments(g_video_id, g_cm + have, MAX_COMMENTS - have, &cursor, err, sizeof(err))
	                     : yt_comments_more(cursor, g_cm + have, MAX_COMMENTS - have, err, sizeof(err));
	if (g_cm_first) g_cm_cursor = cursor;
	if (got < 0) snprintf(g_cm_err, sizeof(g_cm_err), "%s", err);
	if (got <= 0 || !g_cm_cursor) g_cm_end = true;
	__sync_synchronize();   // the comments must be in place before the count says so
	if (got > 0) g_cm_n = have + got;
	tlog("direct: comments: %d more (%d in all)%s%s", got, have + (got > 0 ? got : 0), got < 0 ? ": " : "",
	     got < 0 ? err : "");
	g_cm_busy = false;
}

// Starts fetching the next page of comments, unless there are no more or one is already coming.
static void comments_fetch(void)
{
	if (g_cm_busy || g_cm_end || g_cm_n >= MAX_COMMENTS) return;
	if (g_cm_thread)
	{
		threadJoin(g_cm_thread, U64_MAX);
		threadFree(g_cm_thread);
		g_cm_thread = NULL;
	}
	g_cm_first = !g_cm_started;
	g_cm_started = true;
	g_cm_busy = true;
	yt_comments_allow();   // here rather than in the thread, so a cancel from comments_clear can't be lost
	g_cm_thread = threadCreate(comments_main, NULL, THREAD_STACK, background_prio(), -2, false);
	if (!g_cm_thread)
	{
		g_cm_busy = false;
		g_cm_end = true;
		snprintf(g_cm_err, sizeof(g_cm_err), "Couldn't load the comments.");
	}
}

// ---- like and dislike counts (YouTube only)
// Fetched once per video on their own thread, after playback has settled, and shown on the
// remote's buttons.
static char g_likes[16], g_dislikes[16];   // like "2.6M", or "" until we know
static Thread g_vote_thread;
static bool g_vote_started;

static void votes_main(void* arg)
{
	(void)arg;
	char likes[16], dislikes[16];
	yt_votes(g_video_id, likes, sizeof(likes), dislikes, sizeof(dislikes));
	str_copy(g_dislikes, sizeof(g_dislikes), dislikes);
	str_copy(g_likes, sizeof(g_likes), likes);
	tlog("direct: votes: likes %s, dislikes %s", likes[0] ? likes : "-", dislikes[0] ? dislikes : "-");
}

static void votes_fetch(void)
{
	if (g_vote_started) return;
	g_vote_started = true;
	yt_comments_allow();   // the counts share the comments connection and its cancel flag
	g_vote_thread = threadCreate(votes_main, NULL, THREAD_STACK, background_prio(), -2, false);
}

// Forgets all comments and counts, for a new video or at the end of one. A request still
// running is cancelled.
static void comments_clear(void)
{
	if (g_cm_thread || g_vote_thread) yt_cancel_comments();
	if (g_cm_thread)
	{
		threadJoin(g_cm_thread, U64_MAX);
		threadFree(g_cm_thread);
		g_cm_thread = NULL;
	}
	if (g_vote_thread)
	{
		threadJoin(g_vote_thread, U64_MAX);
		threadFree(g_vote_thread);
		g_vote_thread = NULL;
	}
	g_likes[0] = g_dislikes[0] = 0;
	g_vote_started = false;
	yt_comments_free(g_cm_cursor);
	g_cm_cursor = NULL;
	g_cm_n = 0;
	g_cm_busy = g_cm_first = g_cm_started = g_cm_end = g_cm_open = false;
	g_cm_err[0] = 0;
	pui_comments_reset();
}

#define VRING_BYTES       (2 * 1024 * 1024)     // ~16 s of 480p or ~30 s of 360p video
#define ARING_BYTES       (1536 * 1024)         // ~8.7 s of 44.1 kHz stereo PCM
#define NAL_BUF_SIZE      (YTSRC_MAX_AU + 64)   // the largest access unit ytsrc queues, plus start codes
#define NUM_WAVEBUFS      12                    // ~1.1 s queued at the DSP
#define WAVEBUF_BYTES     (YTSRC_AUDIO_FRAMES * 2 * 2)
#define AUDIO_CHANNEL     0
#define FRAME_QUEUE       8              // decoded pictures kept ready (one access unit can yield up to MVD_OUTS)
#define BACKLIGHT_IDLE_MS 8000           // bottom screen goes dark after this long without input
#define REFRESH_MS        16.713f        // top screen refresh (59.83 Hz)
#define UI_PLAY_MS        200            // bottom-screen remote refresh while playing
#define DEFAULT_FPS_X1000 30000          // frame rate assumed when the stream gives none
#define BIG_PICTURE_BYTES (600 * 1024)   // pictures above this get the smaller pool...
#define BIG_PICTURE_POOL  5              // ...of this many
#define MVD_LINEAR_MARGIN (512 * 1024)   // linear memory left free beside MVD's work buffer
#define SEEK_SHORT_MS     10000          // D-pad Left/Right
#define SEEK_LONG_MS      60000          // L/R
#define SEEK_END_GAP_MS   3000           // a seek lands at least this far before the end
#define STALL_MS          50             // a loop pass this much later than the last counts as a stall
#define STATS_WINDOW_MS   5000           // the playback summary in the log covers this long
#define TOP_SCREEN_W      400.0f         // top screen width in pixels (one eye)
#define CONV_EASE_STEP    0.5f           // auto-convergence moves this many picture pixels per frame
#define MEASURE_EVERY_MS  1000           // 3D analysis (convergence or layout check) once a second
#define LAYOUT_HITS       3              // checks in a row with the same answer settle the layout
#define LAYOUT_MAX_CHECKS 30             // give up after this many textured checks...
#define LAYOUT_MAX_MS     30000          // ...or this long into the session
#define WORK_GUESS_MS     6.0f           // a luma map or motion search's time before any was measured
#define REFRESH_SLACK_MS  1.5f           // time kept free before a refresh for the draw

// ---------------------------------------------------------------- helpers

// ---- 3D comfort: auto-convergence
// Once a second we sample a decoded frame into two small luminance images and measure the
// dominant disparity between the eyes. If the main plane sits too far behind or in front of
// the screen, you see two images that are only sharp in one eye; that happens with badly
// converged uploads or SBS made for a big TV. Such pictures are pulled back into a comfortable
// window. Depth that's already comfortable is left as the author intended.
#define EST_W         160
#define EST_H         90
#define MAX_BEHIND_PX 8.0f              // screen pixels of uncrossed disparity we allow
#define MAX_FRONT_PX  4.0f              // and of crossed (pop-out) disparity
#define DEPTH_STEP_PX 2.0f              // one press of Up/Down
#define DEPTH_STEPS   PUI_DEPTH_STEPS   // presses either way (the settings page's slider has as many dots)

static u8 est_l[EST_W * EST_H], est_r[EST_W * EST_H];
static s8 g_depth_step;   // kept between videos while the app runs

// Returns the right eye's disparity in picture pixels, positive meaning behind the screen.
// *pair is set false when the two halves aren't the same scene (a 2D video mistaken for 3D),
// and *textured false when the frame is too dark or flat to tell.
static float measure_disparity(const u16* frame, u32 stride, u32 iw, u32 ih, VideoLayout layout, bool swap, bool* pair,
                               bool* textured)
{
	u32 eye_w = stereo_sample_eyes(frame, stride, iw, ih, layout == VL_TB, swap, est_l, est_r, EST_W, EST_H);
	int shift = stereo_analyse(est_l, est_r, EST_W, EST_H, 16, pair);
	*textured = stereo_textured(est_l, EST_W, EST_H);
	return (float)shift * (float)eye_w / EST_W;
}

// Call before the CPU reads memory that MVD or the GPU wrote by DMA: the kernel drops this
// process's cached copies on the calling core (and the New 3DS's L2).
// Not GSPGPU_InvalidateDataCache: on hardware it left stale lines, so the worker saw consecutive
// pictures as nearly identical, dropped 150 "repeated" frames in 50 s of a film that has none,
// and measured motion on stale pictures.
static void dma_read_barrier(const void* p, u32 size)
{
	svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)(uintptr_t)p, size);
}

// The smallest power of two, at least 8, that holds v (a texture side).
static u32 pow2_at_least(u32 v)
{
	u32 p = 8;
	while (p < v) p <<= 1;
	return p;
}

// How many reference frames the decoder must keep, from the H.264 level's maximum DPB size
// (table A-1 of the spec).
static u32 dpb_frames(u8 level, u32 w, u32 h)
{
	static const struct {
		u8 level;
		u32 mbs;
	} dpb[] = {
		{ 9, 396 },    { 10, 396 },   { 11, 900 },    { 12, 2376 },   { 13, 2376 },   { 20, 2376 },
		{ 21, 4752 },  { 22, 8100 },  { 30, 8100 },   { 31, 18000 },  { 32, 20480 },  { 40, 32768 },
		{ 41, 32768 }, { 42, 34816 }, { 50, 110400 }, { 51, 184320 }, { 52, 184320 },
	};
	u32 mbs = 18000;   // unknown level: assume 3.1
	for (size_t i = 0; i < sizeof(dpb) / sizeof(dpb[0]); i++)
		if (dpb[i].level == level) mbs = dpb[i].mbs;
	u32 frames = mbs / ((w / 16) * (h / 16));
	if (frames < 1) frames = 1;
	if (frames > 16) frames = 16;
	return frames;
}

// Converts the H.264 level byte from avcC (9 = 1b, 10 = 1.0, ... 52 = 5.2) to MVD's level
// number, or -1 if it's unknown.
static int mvd_level(u8 level)
{
	static const u8 levels[] = { 10, 9, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52 };
	for (int i = 0; i < (int)sizeof(levels); i++)
		if (levels[i] == level) return i;   // MVD_H264_LEVEL_1_0 + i
	return -1;
}

// The size of MVD's work buffer for a w x h stream. We let MVD compute it, as the 3DS browser
// and Video player for 3DS do, both from the H.264 level (with the "extra" sixth for B-frame
// motion data) and from the reference frame count, and it takes the larger.
// Don't size it from the reference frames alone: at 1080p level 4.1 that comes out 2.5 MB
// short, and the MVD module crashed writing past the end (Video player for 3DS issue #69 is
// the same crash). We never go below the 9 MB default, which is plenty for 480p.
static u32 mvd_workbuf_size(u32 w, u32 h, u8 level)
{
	MVDSTD_CalculateWorkBufSizeConfig cfg;
	memset(&cfg, 0, sizeof(cfg));
	int mvd_lvl = mvd_level(level);
	if (mvd_lvl >= 0)
	{
		cfg.level.enable = 1;
		cfg.level.flag = MVD_CALC_WITH_LEVEL_FLAG_ENABLE_CALC | MVD_CALC_WITH_LEVEL_FLAG_ENABLE_EXTRA_OP |
		                 MVD_CALC_WITH_LEVEL_FLAG_UNK;
		cfg.level.level = (u8)mvd_lvl;
	}
	cfg.ref_frames_a.enable = 1;
	cfg.ref_frames_a.ref_frames = (u8)(dpb_frames(level, w, h) + 1);
	cfg.width = w;
	cfg.height = h;
	u32 size = 0;
	if (R_FAILED(mvdstdCalculateBufferSize(&cfg, &size)) || size == 0)
	{
		// The level can't hold this size, or the call failed. Fall back to the maximum of 16
		// reference frames plus the extra sixth.
		cfg.level.enable = 0;
		cfg.ref_frames_a.ref_frames = 16;
		if (R_FAILED(mvdstdCalculateBufferSize(&cfg, &size)) || size == 0) return MVD_DEFAULT_WORKBUF_SIZE;
		size += size / 6;
	}
	return size < MVD_DEFAULT_WORKBUF_SIZE ? MVD_DEFAULT_WORKBUF_SIZE : size;
}

#ifdef __3DS__
static LightLock mvd_lock = 1;   // the value LightLock_Init sets, meaning unlocked
#else
static LightLock mvd_lock = PTHREAD_MUTEX_INITIALIZER;   // on a PC build LightLock is a mutex
#endif
void direct_mvd_lock(void) { LightLock_Lock(&mvd_lock); }
void direct_mvd_unlock(void) { LightLock_Unlock(&mvd_lock); }
u32 direct_mvd_workbuf(u32 w, u32 h, u8 level) { return mvd_workbuf_size(w, h, level); }

// The pts of access units given to MVD but not yet matched to a picture, smallest first.
// MVD returns pictures in display order, so the next picture always gets the smallest pts
// still pending.
#define MAX_PTS 16
typedef struct {
	u32 v[MAX_PTS];
	int n;
} PtsList;

static void pts_add(PtsList* list, u32 pts)
{
	if (list->n == MAX_PTS)
	{
		memmove(list->v, list->v + 1, (MAX_PTS - 1) * sizeof(u32));   // full: forget the oldest
		list->n--;
	}
	int i = list->n;
	while (i > 0 && list->v[i - 1] > pts)
	{
		list->v[i] = list->v[i - 1];
		i--;
	}
	list->v[i] = pts;
	list->n++;
}

static u32 pts_take(PtsList* list)
{
	if (!list->n) return 0;
	u32 smallest = list->v[0];
	memmove(list->v, list->v + 1, (size_t)(list->n - 1) * sizeof(u32));
	list->n--;
	return smallest;
}

// Returns the next packet of the current seek generation. Packets queued before an in-place
// seek (ytsrc_seek) carry an older tag in Pkt.eye, and we drop them here.
static Pkt* peek_cur(Ring* r, u8 tag)
{
	Pkt* p;
	while ((p = ring_peek(r)) != NULL && p->eye != tag) ring_release(r);
	return p;
}

// Milliseconds since the top screen's last refresh, as seen by the loop at vb_tick.
static inline float since_refresh(u64 vb_tick)
{
	return (float)(svcGetSystemTick() - vb_tick) / (float)CPU_TICKS_PER_MSEC;
}

// The average of n timings (summed in CPU ticks) in ms, or fallback while n is 0.
static inline float avg_ms(u64 ticks, u32 n, float fallback)
{
	return n ? (float)((double)ticks / n / CPU_TICKS_PER_MSEC) : fallback;
}

// The layout X switches to: the next one, skipping Auto.
static PlayMode next_mode(PlayMode m) { return m + 1 >= MODE_COUNT ? MODE_2D : (PlayMode)(m + 1); }

// Screen pixels per picture pixel: each eye's picture fills the top screen's width.
static float screen_scale(VideoLayout layout, u32 crop_w)
{
	return TOP_SCREEN_W / (layout == VL_SBS ? crop_w / 2.0f : (float)crop_w);
}

// Moves the auto-convergence one step (CONV_EASE_STEP) towards its target.
static float ease_conv(float conv, float target)
{
	if (conv < target) return conv + CONV_EASE_STEP > target ? target : conv + CONV_EASE_STEP;
	if (conv > target) return conv - CONV_EASE_STEP < target ? target : conv - CONV_EASE_STEP;
	return conv;
}

// The pool slot holding the earliest picture, or -1 if the pool is empty.
static int pool_earliest(const bool* used, const u32* pts)
{
	int next = -1;
	for (int i = 0; i < FRAME_QUEUE; i++)
		if (used[i] && (next < 0 || (s32)(pts[i] - pts[next]) < 0)) next = i;
	return next;
}

static void fmt_time(char* out, size_t len, u32 ms)
{
	u32 secs = ms / 1000;
	if (secs >= 3600)
		snprintf(out, len, "%lu:%02lu:%02lu", (unsigned long)(secs / 3600), (unsigned long)(secs / 60 % 60),
		         (unsigned long)(secs % 60));
	else snprintf(out, len, "%lu:%02lu", (unsigned long)(secs / 60), (unsigned long)(secs % 60));
}

// ---------------------------------------------------------------- stream lookup thread

typedef struct {
	const char* id;
	YtStreams st;
	bool ok;
	volatile bool done;
} Lookup;

static void lookup_main(void* arg)
{
	Lookup* look = (Lookup*)arg;
	look->ok = yt_get_streams(look->id, 480, &look->st);
	__sync_synchronize();   // make st/ok visible before done; the thread may run on another core
	look->done = true;
}

typedef enum { FIND_OK, FIND_FAILED, FIND_CANCELLED } FindResult;

// Asks YouTube for the video's streams on a thread while the loading screen runs. For FIND_OK
// and FIND_FAILED, *out holds the result; free it with yt_free_streams.
static FindResult find_streams(const char* video_id, YtStreams* out, const char* line1)
{
	Lookup look = { .id = video_id, .ok = false, .done = false };
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	Thread th = threadCreate(lookup_main, &look, THREAD_STACK, prio + 1, -1, false);
	if (!th)
	{
		memset(out, 0, sizeof(*out));
		snprintf(out->error, sizeof(out->error), "Couldn't start the network thread.");
		return FIND_FAILED;
	}
	int tick = 0;
	bool cancel = false, closing = false;
	while (!look.done)
	{
		if (closing || !aptMainLoop())
		{
			// the app is closing: stop the request and don't draw anything more
			closing = cancel = true;
			exit_requested("direct: finding streams");
			svcSleepThread(10 * 1000000ULL);
			continue;
		}
		hidScanInput();
		if (hidKeysDown() & KEY_B) cancel = true;
		if (pui_loading(cancel ? "Cancelling..." : line1, "", tick++))   // deliberately just one plain line
			cancel = true;
		if (cancel) yt_cancel_streams();   // repeated every pass, since a cancel sent before the request starts is lost
	}
	__sync_synchronize();
	threadJoin(th, U64_MAX);
	threadFree(th);
	*out = look.st;
	if (cancel)
	{
		yt_free_streams(out);
		return FIND_CANCELLED;
	}
	return look.ok ? FIND_OK : FIND_FAILED;
}

// ---------------------------------------------------------------- subtitles
// The chosen track downloads on a thread (usually a few KB, gzipped) while the video plays.
// Switching tracks never waits for a download: the running one is cancelled, and the next
// starts once it has returned.
#define SUB_MAX_BYTES (3 * 1024 * 1024)   // a 3-hour auto-generated track is ~1.5 MB of XML

// For files from the Videos tab, the captions list holds the .srt/.vtt files next to the video
// (the url is the file's path) and, once the file is open, its own text tracks. Those use the
// url "emb:<n>", with n an index into YtSrc.text, and their cues arrive along with the packets
// (ytsrc_sub_text).
typedef struct {
	int want;   // selected track (index into YtStreams.captions), -1 for off
	int have;   // track loaded into subs (or the embedded track in use), -1 for none
	Subs subs;
	bool emb_listed;    // a file's embedded tracks have been added to the captions list
	int start_choice;   // for a file: the track to start with once they are (-2: Settings decide)
	bool chosen;        // picked from the list during this video (remembered for the file)
	int failed;         // track whose download failed, not retried until picked again; -1 if none
	// the download
	Thread th;
	int loading;
	char* url;
	volatile bool cancel, done;
	bool ok;
	Subs fresh;
} SubState;

// Reads a whole subtitle file (up to SUB_MAX_BYTES) from the SD card or a network place.
// Returns NULL on failure.
static u8* read_sub_file(const char* url, size_t* len, volatile bool* cancel)
{
	u8* buf = (u8*)malloc(SUB_MAX_BYTES);
	if (!buf) return NULL;
	size_t got = 0;
	bool ok = false;
	if (ytsrc_is_local(url))
	{
		FILE* f = fopen(url, "rb");
		if (f)
		{
			got = fread(buf, 1, SUB_MAX_BYTES, f);
			ok = !ferror(f);
			fclose(f);
		}
	}
	else
	{
		char err[160];
		NetFile* f = nf_open(url, 256 * 1024, err, sizeof(err));
		if (f)
		{
			int chunk;
			while (got < SUB_MAX_BYTES && (chunk = nf_read(f, buf + got, (int)(SUB_MAX_BYTES - got), cancel)) > 0)
				got += (size_t)chunk;
			ok = !nf_error(f, NULL, 0) && !*cancel;
			nf_close(f);
		}
		else tlog("direct: subtitle file: %s", err);
	}
	if (!ok)
	{
		free(buf);
		return NULL;
	}
	*len = got;
	return buf;
}

static void sub_thread(void* arg)
{
	SubState* s = (SubState*)arg;
	s->ok = false;
	if (ytsrc_is_file(s->url))
	{
		size_t len = 0;
		const u64 start_ms = osGetTime();
		u8* data = read_sub_file(s->url, &len, &s->cancel);
		tlog("direct: subtitle file: %lu KB in %llu ms", (unsigned long)(len / 1024),
		     (unsigned long long)(osGetTime() - start_ms));
		if (data) s->ok = subs_parse_file((const char*)data, len, &s->fresh);
		free(data);
		__sync_synchronize();   // make fresh/ok visible before done
		s->done = true;
		return;
	}
	void* http = yt_http_new();
	u8* buf = (u8*)malloc(SUB_MAX_BYTES);
	size_t len = 0;
	const u64 start_ms = osGetTime();
	const long status = http && buf ? yt_http_get_ex(http, s->url, buf, SUB_MAX_BYTES, &len, &s->cancel) : -1;
	tlog("direct: subtitle track: HTTP %ld, %lu KB in %llu ms", status, (unsigned long)(len / 1024),
	     (unsigned long long)(osGetTime() - start_ms));
	if (status == 200) s->ok = subs_parse((const char*)buf, len, &s->fresh);
	free(buf);
	yt_http_free(http);
	__sync_synchronize();   // make fresh/ok visible before done
	s->done = true;
}

// Picks the track to start with when subtitles are switched on in Settings: one in the
// console's language (the uploader's own before an auto-generated one), else the first track,
// or -1 if there are none.
static int sub_default(const YtStreams* st)
{
	static const char* langs[] = { "ja", "en", "fr", "de", "it", "es", "zh", "ko", "nl", "pt", "ru", "zh" };
	u8 lang = CFG_LANGUAGE_EN;
	if (R_SUCCEEDED(cfguInit()))
	{
		CFGU_GetSystemLanguage(&lang);
		cfguExit();
	}
	const char* want = lang < sizeof(langs) / sizeof(langs[0]) ? langs[lang] : "en";
	size_t want_len = strlen(want);
	for (int i = 0; i < st->caption_count; i++)   // uploader tracks come first
		if (!strncmp(st->captions[i].lang, want, want_len) &&
		    (st->captions[i].lang[want_len] == 0 || st->captions[i].lang[want_len] == '-'))
			return i;
	return st->caption_count ? 0 : -1;
}

// Called on every loop pass: picks up a finished download and starts the one that's wanted.
static void sub_update(SubState* s, const YtStreams* st, bool may_start)
{
	if (s->th && s->done)
	{
		threadJoin(s->th, U64_MAX);
		threadFree(s->th);
		s->th = NULL;
		free(s->url);
		s->url = NULL;
		__sync_synchronize();
		if (s->ok && !s->cancel && s->loading == s->want)
		{
			subs_free(&s->subs);   // a downloaded track's cues (an embedded track keeps none here)
			s->subs = s->fresh;
			s->have = s->loading;
			tlog("direct: subtitles %s: %d cues", st->captions[s->loading].lang, s->subs.n);
		}
		else
		{
			if (!s->ok && !s->cancel)
			{
				s->failed = s->loading;
				tlog("direct: subtitles %s couldn't be loaded", st->captions[s->loading].lang);
			}
			subs_free(&s->fresh);
		}
		memset(&s->fresh, 0, sizeof(s->fresh));
	}
	if (s->th && s->loading != s->want) s->cancel = true;   // another track was picked in the meantime
	if (s->want >= 0 && s->want < st->caption_count && !strncmp(st->captions[s->want].url, "emb:", 4))
	{
		s->have = s->want;   // a track inside the file: nothing to download
		return;
	}
	if (may_start && !s->th && s->want >= 0 && s->want < st->caption_count && s->want != s->have &&
	    s->want != s->failed)
	{
		s->url = strdup(st->captions[s->want].url);
		s->loading = s->want;
		s->cancel = s->done = false;
		// below the decoder too (background_prio), so its TLS handshake only uses idle time
		if (s->url) s->th = threadCreate(sub_thread, s, THREAD_STACK, background_prio(), -1, false);
		if (!s->th)
		{
			free(s->url);
			s->url = NULL;
			s->failed = s->want;
		}
	}
}

// Selects a subtitle track by its index in st->captions, or -1 for off.
static void sub_choose(SubState* s, const YtStreams* st, int track)
{
	s->want = track >= 0 && track < st->caption_count ? track : -1;
	s->failed = -1;   // choosing a track again retries it
	s->chosen = true;
	tlog("direct: subtitles -> %s", s->want < 0 ? "off" : st->captions[s->want].lang);
}

static void sub_stop(SubState* s)
{
	if (s->th)
	{
		s->cancel = true;
		threadJoin(s->th, U64_MAX);   // the transfer notices within about a second
		threadFree(s->th);
		s->th = NULL;
		subs_free(&s->fresh);
	}
	free(s->url);
	s->url = NULL;
	subs_free(&s->subs);
}

// ---------------------------------------------------------------- a file's tracks

// Language codes as video files write them (ISO 639-1 and both 639-2 forms), plus the names
// people put in .srt file names ("Movie.en.srt", "Movie.eng.srt", "Movie.English.srt").
static const struct {
	const char* c2;
	const char* c3a;
	const char* c3b;
	const char* name;
} langs[] = {
	{ "en", "eng", "eng", "English" },    { "fr", "fre", "fra", "French" },     { "de", "ger", "deu", "German" },
	{ "es", "spa", "spa", "Spanish" },    { "it", "ita", "ita", "Italian" },    { "ja", "jpn", "jpn", "Japanese" },
	{ "zh", "chi", "zho", "Chinese" },    { "ko", "kor", "kor", "Korean" },     { "nl", "dut", "nld", "Dutch" },
	{ "pt", "por", "por", "Portuguese" }, { "ru", "rus", "rus", "Russian" },    { "hi", "hin", "hin", "Hindi" },
	{ "ar", "ara", "ara", "Arabic" },     { "sv", "swe", "swe", "Swedish" },    { "no", "nor", "nob", "Norwegian" },
	{ "da", "dan", "dan", "Danish" },     { "fi", "fin", "fin", "Finnish" },    { "pl", "pol", "pol", "Polish" },
	{ "tr", "tur", "tur", "Turkish" },    { "el", "gre", "ell", "Greek" },      { "he", "heb", "heb", "Hebrew" },
	{ "cs", "cze", "ces", "Czech" },      { "hu", "hun", "hun", "Hungarian" },  { "th", "tha", "tha", "Thai" },
	{ "vi", "vie", "vie", "Vietnamese" }, { "id", "ind", "ind", "Indonesian" }, { "ml", "mal", "mal", "Malayalam" },
	{ "ta", "tam", "tam", "Tamil" },      { "te", "tel", "tel", "Telugu" },     { "bn", "ben", "ben", "Bengali" },
	{ "uk", "ukr", "ukr", "Ukrainian" },  { "ro", "rum", "ron", "Romanian" },   { "ms", "may", "msa", "Malay" },
};

// Looks up a language code or name and returns its index in langs, or -1 if unknown.
static int lang_index(const char* code, size_t n)
{
	for (size_t i = 0; i < sizeof(langs) / sizeof(langs[0]); i++)
	{
		const char* names[4] = { langs[i].c2, langs[i].c3a, langs[i].c3b, langs[i].name };
		for (int k = 0; k < 4; k++)
			if (strlen(names[k]) == n && !strncasecmp(code, names[k], n)) return (int)i;
	}
	return -1;
}

// A readable language name like "English". Unknown codes are shown as they are, and an empty
// code gives the fallback.
static const char* lang_label(const char* code, const char* fallback)
{
	const int i = code[0] ? lang_index(code, strlen(code)) : -1;
	return i >= 0 ? langs[i].name : code[0] ? code : fallback;
}

static void caption_lang(YtCaption* c, const char* code, size_t n)
{
	const int i = lang_index(code, n);
	if (i >= 0) snprintf(c->lang, sizeof(c->lang), "%s", langs[i].c2);   // sub_default compares these
	else snprintf(c->lang, sizeof(c->lang), "%.*s", (int)(n < 8 ? n : 8), code);
}

static bool has_sub_ext(const char* name, size_t* stem)
{
	const size_t n = strlen(name);
	if (n < 5 || (strcasecmp(name + n - 4, ".srt") != 0 && strcasecmp(name + n - 4, ".vtt") != 0)) return false;
	*stem = n - 4;
	return true;
}

// Adds the .srt and .vtt files next to a video to the captions list. For "Movie.mkv" that
// would be names like Movie.srt, Movie.en.srt or Movie.eng.forced.srt.
static void add_sidecar_subs(YtStreams* st, const char* path)
{
	const char* slash = strrchr(path, '/');
	if (!slash) return;
	const char* base = slash + 1;
	const char* dot = strrchr(base, '.');
	const size_t base_len = dot ? (size_t)(dot - base) : strlen(base);
	char dir[400];
	snprintf(dir, sizeof(dir), "%.*s", (int)(slash + 1 - path), path);
	enum { MAX_NAMES = 160 };
	NfEntry* names = (NfEntry*)malloc(sizeof(NfEntry) * MAX_NAMES);
	if (!names) return;
	int n = 0;
	if (ytsrc_is_local(path))
	{
		DIR* d = opendir(dir);
		struct dirent* e;
		while (d && n < MAX_NAMES && (e = readdir(d)) != NULL)
			snprintf(names[n++].name, sizeof(names[0].name), "%s", e->d_name);
		if (d) closedir(d);
	}
	else
	{
		char err[160];
		n = nf_list(dir, names, MAX_NAMES, err, sizeof(err));
		if (n < 0)
		{
			tlog("direct: subtitle files: %s", err);
			n = 0;
		}
	}
	for (int i = 0; i < n && st->caption_count < YT_MAX_CAPTIONS; i++)
	{
		const char* name = names[i].name;
		size_t stem;
		if (!has_sub_ext(name, &stem) || stem < base_len || strncasecmp(name, base, base_len) != 0) continue;
		if (stem > base_len && name[base_len] != '.') continue;   // "Movie 2.srt" belongs to another video
		YtCaption* c = &st->captions[st->caption_count];
		memset(c, 0, sizeof(*c));
		// the part between the video's name and the extension: a language, "forced", "sdh"...
		const char* tag = stem > base_len ? name + base_len + 1 : "";
		const size_t tag_len = stem > base_len ? stem - base_len - 1 : 0;
		const size_t lang_len = strcspn(tag, ".") < tag_len ? strcspn(tag, ".") : tag_len;
		caption_lang(c, tag, lang_len);
		const int lang = lang_index(tag, lang_len);
		if (lang >= 0)
			snprintf(c->name, sizeof(c->name), "%s%s%.*s  \xC2\xB7  file", langs[lang].name,
			         tag_len > lang_len ? " " : "", (int)(tag_len > lang_len ? tag_len - lang_len - 1 : 0),
			         tag + lang_len + 1);
		else snprintf(c->name, sizeof(c->name), "%.40s  \xC2\xB7  file", name);
		const size_t url_len = strlen(dir) + strlen(name) + 1;
		c->url = (char*)malloc(url_len);
		if (!c->url) break;
		snprintf(c->url, url_len, "%s%s", dir, name);
		st->caption_count++;
		tlog("direct: subtitle file %s (%s)", name, c->lang);
	}
	free(names);
}

// Adds a file's own text subtitle tracks to the captions list, after the .srt files.
static void add_embedded_subs(YtStreams* st, const YtSrc* src)
{
	for (int t = 0; t < src->n_text && st->caption_count < YT_MAX_CAPTIONS; t++)
	{
		const YtSrcTrack* tr = &src->text[t];
		YtCaption* c = &st->captions[st->caption_count];
		memset(c, 0, sizeof(*c));
		caption_lang(c, tr->lang, strlen(tr->lang));
		char fallback[16];
		snprintf(fallback, sizeof(fallback), "Track %d", t + 1);
		snprintf(c->name, sizeof(c->name), "%.20s%s%.30s%s%s", lang_label(tr->lang, fallback), tr->title[0] ? " (" : "",
		         tr->title, tr->title[0] ? ")" : "", tr->forced ? " forced" : "");
		c->url = (char*)malloc(16);
		if (!c->url) break;
		snprintf(c->url, 16, "emb:%d", t);
		st->caption_count++;
	}
}

// Names an audio track for the list, e.g. "English (Commentary)  ·  AC3 5.1".
static void audio_track_name(const YtSrcTrack* tr, int i, char* out, size_t cap)
{
	char fallback[16], channels[12], codec[12];
	snprintf(fallback, sizeof(fallback), "Track %d", i + 1);
	if (tr->channels == 1) snprintf(channels, sizeof(channels), "mono");
	else if (tr->channels == 2) snprintf(channels, sizeof(channels), "stereo");
	else if (tr->channels == 6) snprintf(channels, sizeof(channels), "5.1");
	else if (tr->channels == 8) snprintf(channels, sizeof(channels), "7.1");
	else snprintf(channels, sizeof(channels), "%u ch", (unsigned)tr->channels);
	size_t k = 0;
	for (; tr->codec[k] && k + 1 < sizeof(codec); k++) codec[k] = (char)toupper((unsigned char)tr->codec[k]);
	codec[k] = 0;
	snprintf(out, cap, "%.20s%s%.30s%s  \xC2\xB7  %s %s", lang_label(tr->lang, fallback), tr->title[0] ? " (" : "",
	         tr->title, tr->title[0] ? ")" : "", codec, channels);
}

// ---------------------------------------------------------------- MVD worker thread
// The hardware decoder's calls block the calling thread, about 11 ms per picture and up to
// 40 ms for a keyframe, while the CPU just waits. If the playback loop made those calls, any
// that ran past a screen refresh would leave that refresh without a picture. With Smooth
// motion drawing at every refresh, that's a visible stutter several times a second (about 9%
// of refreshes on hardware). So the loop hands each access unit to this thread and keeps
// drawing, and does the rest once the pictures are ready.
//
// For Smooth motion the thread also builds each new picture's luma map and measures its
// motion from the previous picture. It runs below the loop's priority, so this only uses time
// the loop doesn't need. There is only ever one job in flight, and the loop touches the
// decoder itself (init/exit on seeks and HOME) only while the thread is idle.
//
// How MVD hands out pictures (learned from Core-2-Extreme's Video player for 3DS and gbatemp
// thread 586094, page 20): MVD_STATUS_FRAMEREADY doesn't mean a new picture was written. A
// picture can be written during ProcessNALUnit itself, into the output of the last
// configuration, and more may be waiting after that. Each of those is fetched by another
// render call into a fresh buffer, until a call writes nothing. The only way to tell whether
// a call wrote a picture is to put marker bytes in the output buffer's corners and check them.
// Assuming one picture per access unit is wrong: on hardware a 24 fps trailer showed only
// about 13 distinct pictures a second. When nothing was written the buffer still held the old
// picture (a repeat), and when two were waiting one got lost, so the P frames of an IBBP
// stream never reached the screen.
#define MVD_OUTS         3      // pictures one access unit can give (each needs a free pool slot)
#define NAL_TYPE_MASK    0x1F   // H.264 NAL unit header: the type bits
#define NAL_SPS          7      // sequence parameter set
#define NAL_PPS          8      // picture parameter set
#define MVD_MARKER       0x11   // the byte put in an output buffer's corners (see mvd_arm)
#define MVD_RENDER_TRIES 200    // render calls while MVD answers busy, before giving up
#define GPU_WAIT_MS      200    // longest wait for the GPU before a copy (pui_gpu_begin)

// The 3D analysis a job may ask for, once a second. It takes about 15-20 ms, which in the
// playback loop would cost a refresh every time: a one-refresh freeze of Smooth motion each
// second while the layout is undecided, and for the whole video in 3D (convergence).
enum { MEASURE_NONE, MEASURE_CONV, MEASURE_SBS, MEASURE_TB };

typedef struct {
	int buf;                  // which output buffer holds it
	bool luma_ok, field_ok;   // its luma map and its motion from the previous picture are ready
	bool repeat;              // it repeats the last picture kept, so drop it
} WkOut;

typedef struct {
	Thread th;
	LightEvent go;
	volatile bool quit;
	volatile bool busy;   // set by the loop when it posts a job, cleared when it takes the result
	volatile bool done;   // set by the thread when the job is finished
	// the job
	u8* nal;
	u32 len;
	bool first;   // the first access unit of this decoder session
	MVDSTD_Config* cfg;
	u8* outs[MVD_OUTS];   // MVD's output buffers (linear, coded size)
	u32 frame_bytes, coded_w;
	bool want_motion;    // Smooth motion is on
	bool want_repeats;   // look for repeats from frame-rate conversion (30 fps and up only)
	int stride, pic_w, pic_h;
	int measure;   // 3D analysis of the first picture, a MEASURE_* (stereo.c, ~15 ms)
	VideoLayout measure_layout;
	bool measure_swap;
	u32 crop_w, crop_h;
	// the results
	Result r;
	int n_out;                    // pictures this access unit gave, in display order
	bool m_done, m_pair, m_tex;   // the 3D analysis ran, and its stereo-pair and texture verdicts
	float m_disp;                 // and the disparity it found (picture pixels)
	WkOut out[MVD_OUTS];
	int lw, lh;
	u64 dec_ticks, luma_ticks, me_ticks;
	// decoder and motion state; it belongs to the thread, and the loop resets it only while idle
	int cfg_buf;   // the output buffer in MVD's current configuration (-1: none yet)
	bool prev_ok, prev_field_ok;
	MeRepeat rep;
	u32 written_in_process, written_by_render, renders;   // statistics for the log
} MvdWorker;

static MvdWorker wk;
static u8 wk_luma[MVD_OUTS][ME_MAX_W * ME_MAX_H], wk_prev_luma[ME_MAX_W * ME_MAX_H];
static MotionField wk_field[MVD_OUTS], wk_prev_field;

// Marker bytes go in the four corners of an output buffer, as Video player for 3DS does: if
// any of them changed, MVD wrote a picture there. The CPU writes them and flushes its cache
// before MVD writes by DMA, then invalidates again before reading them back.
static void mvd_corners(u32* at)
{
	at[0] = 0;
	at[1] = wk.coded_w * 2 - 1;
	at[2] = wk.frame_bytes - wk.coded_w * 2;
	at[3] = wk.frame_bytes - 1;
}

static void mvd_arm(u8* buf)
{
	u32 at[4];
	mvd_corners(at);
	for (int i = 0; i < 4; i++)
	{
		buf[at[i]] = MVD_MARKER;
		svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)(uintptr_t)(buf + at[i]), 1);
	}
}

static bool mvd_written(u8* buf)
{
	u32 at[4];
	mvd_corners(at);
	bool written = false;
	for (int i = 0; i < 4; i++)
	{
		svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)(uintptr_t)(buf + at[i]), 1);
		written = written || buf[at[i]] != MVD_MARKER;
	}
	return written;
}

// Points MVD's output at buffer i and asks for a picture there; libctru only applies a new
// configuration together with a render call. Returns true if a picture was written.
static bool mvd_render_into(int i)
{
	u8* buf = wk.outs[i];
	mvd_arm(buf);
	wk.cfg->physaddr_outdata0 = osConvertVirtToPhys(buf);
	wk.cfg_buf = i;
	for (int k = 0; k < MVD_RENDER_TRIES; k++)
	{
		Result res = mvdstdRenderVideoFrame(wk.cfg, false);
		wk.renders++;
		if (mvd_written(buf)) return true;
		if (res != MVD_STATUS_BUSY) return false;
	}
	return false;
}

static int mvd_free_buf(void)
{
	for (int i = 0; i < MVD_OUTS; i++)
	{
		bool used = false;
		for (int k = 0; k < wk.n_out; k++) used = used || wk.out[k].buf == i;
		if (!used) return i;
	}
	return -1;
}

static void mvd_add(int i)
{
	wk.out[wk.n_out].buf = i;
	wk.out[wk.n_out].luma_ok = wk.out[wk.n_out].field_ok = wk.out[wk.n_out].repeat = false;
	wk.n_out++;
}

static void mvd_decode(void)
{
	wk.n_out = 0;
	if (wk.cfg_buf < 0)
		mvd_render_into(0);   // give MVD a configuration before the first access unit (nothing to fetch yet)
	if (wk.first)
	{
		// Like FourthTube: feed the parameter sets on their own first, then the first frame
		// twice.
		for (u32 start = 0; start + 4 < wk.len;)
		{
			u32 end = start + 3;   // the next start code (00 00 01), or the end of the access unit
			while (end + 3 < wk.len && !(wk.nal[end] == 0 && wk.nal[end + 1] == 0 && wk.nal[end + 2] == 1)) end++;
			if (end + 3 >= wk.len) end = wk.len;
			u8 type = wk.nal[start + 3] & NAL_TYPE_MASK;
			if (type != NAL_SPS && type != NAL_PPS) break;
			mvdstdProcessVideoFrame(wk.nal + start, end - start, 0, NULL);
			start = end;
		}
		mvdstdProcessVideoFrame(wk.nal, wk.len, 0, NULL);
	}
	const int cfg_out = wk.cfg_buf;
	mvd_arm(wk.outs[cfg_out]);
	wk.r = mvdstdProcessVideoFrame(wk.nal, wk.len, 0, NULL);
	if (mvd_written(wk.outs[cfg_out]))
	{
		// A picture was written while processing, and more may be waiting. Fetch those into
		// fresh buffers.
		wk.written_in_process++;
		mvd_add(cfg_out);
		while (wk.n_out < MVD_OUTS)
		{
			int free_buf = mvd_free_buf();
			if (free_buf < 0 || !mvd_render_into(free_buf)) break;
			wk.written_by_render++;
			mvd_add(free_buf);
		}
	}
	else if (mvd_render_into(cfg_out))
	{
		wk.written_by_render++;
		mvd_add(cfg_out);
	}
}

static void mvd_worker_main(void* arg)
{
	(void)arg;
	while (true)
	{
		LightEvent_Wait(&wk.go);
		if (wk.quit) break;
		u64 decode_start = svcGetSystemTick();
		mvd_decode();
		wk.dec_ticks = svcGetSystemTick() - decode_start;
		wk.m_done = false;
		if (wk.measure != MEASURE_NONE && wk.n_out > 0)
		{
			u8* frame = wk.outs[wk.out[0].buf];
			dma_read_barrier(frame, wk.frame_bytes);   // written by MVD's DMA
			const VideoLayout layout = wk.measure == MEASURE_SBS  ? VL_SBS
			                           : wk.measure == MEASURE_TB ? VL_TB
			                                                      : wk.measure_layout;
			wk.m_disp = measure_disparity((const u16*)frame, wk.coded_w, wk.crop_w, wk.crop_h, layout,
			                              wk.measure == MEASURE_CONV && wk.measure_swap, &wk.m_pair, &wk.m_tex);
			wk.m_done = true;
		}
		wk.luma_ticks = wk.me_ticks = 0;
		for (int o = 0; o < wk.n_out && wk.want_motion; o++)
		{
			WkOut* wo = &wk.out[o];
			u8* frame = wk.outs[wo->buf];
			u64 luma_start = svcGetSystemTick();
			dma_read_barrier(frame, wk.frame_bytes);   // written by MVD's DMA
			me_luma((const u16*)frame, wk.stride, wk.pic_w, wk.pic_h, wk_luma[o], &wk.lw, &wk.lh);
			wo->luma_ok = true;
			u64 me_start = svcGetSystemTick();
			wk.luma_ticks += me_start - luma_start;
			// A repeated frame (24/25 fps converted to 30, see me_repeat) gets dropped by the
			// loop, so the next picture's motion is measured from the last one kept.
			wo->repeat =
			    wk.want_repeats && wk.prev_ok && me_repeat(me_diff(wk_prev_luma, wk_luma[o], wk.lw, wk.lh), &wk.rep);
			if (!wo->repeat)
			{
				if (wk.prev_ok)
				{
					me_estimate(wk_prev_luma, wk_luma[o], wk.lw, wk.lh, wk.prev_field_ok ? &wk_prev_field : NULL,
					            &wk_field[o]);
					wk_prev_field = wk_field[o];
					wk.prev_field_ok = wo->field_ok = true;
					wk.me_ticks += svcGetSystemTick() - me_start;
				}
				memcpy(wk_prev_luma, wk_luma[o], (size_t)wk.lw * wk.lh);
				wk.prev_ok = true;
			}
		}
		__sync_synchronize();   // make the results visible before done
		wk.done = true;
	}
}

static bool mvd_worker_start(void)
{
	memset(&wk, 0, sizeof(wk));
	wk.cfg_buf = -1;   // no configuration given to MVD yet
	LightEvent_Init(&wk.go, RESET_ONESHOT);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// Run below the playback loop on its core, so drawing always comes first and the thread
	// gets the time the loop spends waiting for refreshes.
	wk.th = threadCreate(mvd_worker_main, NULL, 32 * 1024, prio + 1, -2, false);
	return wk.th != NULL;
}

// Waits for any job in flight to finish, so the decoder can be touched safely (on a seek,
// HOME or the end).
static void mvd_worker_wait_idle(void)
{
	while (wk.busy && !wk.done) svcSleepThread(1000000ULL);
	__sync_synchronize();
}

static void mvd_worker_stop(void)
{
	if (!wk.th) return;
	mvd_worker_wait_idle();
	wk.quit = true;
	LightEvent_Signal(&wk.go);
	threadJoin(wk.th, U64_MAX);
	threadFree(wk.th);
	wk.th = NULL;
}

// ---------------------------------------------------------------- one playback session

// What to do when a session ends. D_MUXED plays the muxed 360p stream instead of the DASH pair
// (the picture was too big for a texture, or the DASH links were refused). D_REFRESH means the
// links expired or failed, so ask YouTube again first. D_AUDIO switches to another audio
// track. All of these resume at *pos_ms.
typedef enum { D_STOP, D_SEEK, D_ENDED, D_MUXED, D_REFRESH, D_AUDIO } DAction;

typedef struct {
	u8* nal;   // linear, NAL_BUF_SIZE
	u8* vring;
	u8* aring;
	u8* frame;                // the MVD output being handled (one of outs)
	u8* outs[MVD_OUTS];       // MVD's output buffers in linear memory, coded w x h BGR565
	u8* tiled[FRAME_QUEUE];   // decoded pictures waiting for the screen, GPU-tiled, in linear memory
	int pool;                 // how many of them there are (fewer for big files, to save linear memory)
	u32 frame_bytes;          // size of one MVD output, and of one pooled picture
	C3D_Tex tex[2];
	bool tex_ok;
	u16 tex_w, tex_h;
} Buffers;

static void free_frame_buffers(Buffers* b)
{
	if (b->tex_ok)
	{
		C3D_TexDelete(&b->tex[0]);
		C3D_TexDelete(&b->tex[1]);
		b->tex_ok = false;
	}
	for (int i = 0; i < MVD_OUTS; i++)
	{
		if (b->outs[i]) linearFree(b->outs[i]);
		b->outs[i] = NULL;
	}
	for (int i = 0; i < FRAME_QUEUE; i++)
	{
		if (b->tiled[i]) linearFree(b->tiled[i]);
		b->tiled[i] = NULL;
	}
	b->frame = NULL;
	b->frame_bytes = 0;
}

// w x h is the size of MVD's pictures, which is also the size shown.
static bool alloc_frame_buffers(Buffers* b, u32 w, u32 h, int pool)
{
	u32 bytes = w * h * 2;
	if (b->outs[0] && b->frame_bytes == bytes && b->tex_w == pow2_at_least(w) && b->tex_h == pow2_at_least(h) &&
	    b->pool == pool)
		return true;   // same size as the last session
	free_frame_buffers(b);
	bool all_ok = true;
	for (int i = 0; i < MVD_OUTS; i++) all_ok = (b->outs[i] = (u8*)linearMemAlign(bytes, 0x80)) != NULL && all_ok;
	b->frame = b->outs[0];
	for (int i = 0; i < pool; i++) all_ok = (b->tiled[i] = (u8*)linearMemAlign(bytes, 0x80)) != NULL && all_ok;
	b->pool = pool;
	b->frame_bytes = bytes;
	b->tex_w = (u16)pow2_at_least(w);
	b->tex_h = (u16)pow2_at_least(h);
	if (!all_ok || b->tex_w > 1024 || b->tex_h > 1024)
	{
		free_frame_buffers(b);
		return false;
	}
	for (int i = 0; i < 2; i++)
	{
		// VRAM keeps the textures out of the linear heap; fall back to linear memory if VRAM is full
		if (!C3D_TexInitVRAM(&b->tex[i], b->tex_w, b->tex_h, GPU_RGB565) &&
		    !C3D_TexInit(&b->tex[i], b->tex_w, b->tex_h, GPU_RGB565))
		{
			if (i == 1) C3D_TexDelete(&b->tex[0]);
			free_frame_buffers(b);
			return false;
		}
		C3D_TexSetFilter(&b->tex[i], GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(&b->tex[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	}
	b->tex_ok = true;
	return true;
}

// Copies a decoded MVD frame (linear, w x h) into a queue slot in the GPU's tiled layout,
// entirely on the GPU. It never scales: shrinking in this transfer hung the GPU on hardware,
// so MVD outputs the picture at the size shown (see run_session).
static void queue_frame(Buffers* b, int slot, u32 w, u32 h)
{
	// No vertical flip: the texture's first tile row is its top (v = 1), the same layout as
	// the thumbnails. Flipping here shows the picture upside down.
	if (!pui_gpu_begin(GPU_WAIT_MS)) return;
	C3D_SyncDisplayTransfer((u32*)b->frame, GX_BUFFER_DIM(w, h), (u32*)b->tiled[slot], GX_BUFFER_DIM(w, h),
	                        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
	                            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
	                            GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
	                            GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
	pui_gpu_end();
	// make sure the copy is done before MVD writes the next picture into the same buffer
	if (pui_gpu_begin(GPU_WAIT_MS)) pui_gpu_end();
}

// Copies a queue slot into the texture when its turn on screen comes.
static void show_frame(Buffers* b, int slot, C3D_Tex* tex, u32 w, u32 h)
{
	// The tiled data is h/8 rows of 8x8 tiles, w*16 bytes per row; in the texture those rows
	// are tex_w*16 bytes apart. TextureCopy dimensions are in 16-byte units.
	u32 row = w * 16, tex_row = (u32)b->tex_w * 16;
	if (!pui_gpu_begin(GPU_WAIT_MS)) return;
	C3D_SyncTextureCopy((u32*)b->tiled[slot], GX_BUFFER_DIM(row >> 4, 0), (u32*)tex->data,
	                    GX_BUFFER_DIM(row >> 4, (tex_row - row) >> 4), row * (h / 8), 8);
	pui_gpu_end();   // submits it; the next draw's begin waits for it to finish
}

// Plays one session of a video, from *pos_ms until something ends it (see DAction).
// audio_stream is a file's audio track (YtSrcTrack.stream, -1 for its default); on D_AUDIO it
// is set to the newly chosen track.
static DAction run_session(YtStreams* st, const char* title, PlayMode* mode, u32* pos_ms, bool audio_ok, Buffers* b,
                           bool use_muxed, SubState* subs, int* audio_stream)
{
	DAction action = D_STOP;
	char err[256];
	Ring video_ring, audio_ring;
	ring_init(&video_ring, b->vring, VRING_BYTES);
	ring_init(&audio_ring, b->aring, ARING_BYTES);
	YtSrc src;
	bool dash = !use_muxed && st->video_url && st->audio_url;
	const bool can_fall_back = dash && st->muxed_url;   // the 640x360 muxed stream always fits
	if (!ytsrc_start(&src, &video_ring, &audio_ring, dash ? st->video_url : NULL, dash ? st->audio_url : NULL,
	                 st->muxed_url, *pos_ms, *audio_stream))
	{
		pui_message("Something went wrong", "Couldn't start the download thread.", NULL, "Back");
		return D_STOP;
	}

	// ---- wait until the source has opened the stream
	int tick = 0;
	bool cancelled = false;
	while (!ytsrc_is_ready(&src) && !ytsrc_has_failed(&src))
	{
		if (!aptMainLoop())
		{
			exit_requested("direct: loading");
			cancelled = true;
			break;
		}
		hidScanInput();
		if (hidKeysDown() & KEY_B)
		{
			cancelled = true;
			break;
		}
		if (pui_loading("Loading video", ytsrc_is_file(st->muxed_url) ? "Opening the file..." : "", tick++))
		{
			cancelled = true;
			break;
		}
	}
	if (cancelled || !ytsrc_is_ready(&src))
	{
		if (!cancelled && ytsrc_has_failed(&src))
		{
			snprintf(err, sizeof(err), "%s", src.errmsg);
			tlog("direct: source failed: %s%s", err, can_fall_back ? " (trying the muxed stream)" : "");
			ytsrc_stop(&src);
			if (can_fall_back) return D_MUXED;
			// Retry asks YouTube for fresh links, since the old ones may have expired.
			return pui_message("Video couldn't be played", err, "Retry", "Back") ? D_REFRESH : D_STOP;
		}
		ytsrc_stop(&src);
		return D_STOP;
	}

	// For a file: add its own subtitle tracks to the list (once), and take its length and audio tracks.
	if (ytsrc_is_file(st->muxed_url))
	{
		if (!subs->emb_listed)
		{
			subs->emb_listed = true;
			add_embedded_subs(st, &src);
			const int c = subs->start_choice;
			subs->want = c >= 0 && c < st->caption_count ? c : c == -2 && app.subtitles ? sub_default(st) : -1;
			tlog("direct: %d subtitle tracks (%d in the file), starting with %s", st->caption_count, src.n_text,
			     subs->want < 0 ? "none" : st->captions[subs->want].name);
			for (int a = 0; a < src.n_audio; a++)
				tlog("direct: audio track %d: stream %d %s %s %u ch%s", a, src.audio[a].stream, src.audio[a].lang,
				     src.audio[a].codec, (unsigned)src.audio[a].channels, a == src.audio_cur ? " (playing)" : "");
		}
		if (!st->duration_ms) st->duration_ms = src.duration_ms;
		if (src.audio_cur >= 0) *audio_stream = src.audio[src.audio_cur].stream;
	}

	// The stream's coded size, and the size of the picture MVD hands out for the screen. A
	// stream wider or taller than a 1024-px texture (720p and 1080p files) is shrunk. MVD does
	// that for free while converting colours, through its configuration's output size. Don't
	// shrink in the GPU's tiling transfer instead (linear in, tiled out, scaled): that hung the
	// GPU on hardware.
	const u32 stream_w = src.width, stream_h = src.height;
	// 1080p is shown at a third, 640x360. MVD's decode time doesn't depend on its output size,
	// and the small pictures let the whole pool fit in the linear heap next to the ~18 MB work
	// buffer, and fit Smooth's motion maps. It's still more than the 400x240 screen shows.
	const u32 vscale = stream_w > 1280 || stream_h > 736 ? 3 : stream_w > 1024 || stream_h > 1024 ? 2 : 1;
	const u32 w = (stream_w / vscale) & ~7u, h = (stream_h / vscale) & ~7u;   // multiples of 8 (the GPU's tiles)
	const u32 crop_w = src.crop_w / vscale, crop_h = src.crop_h / vscale;     // the visible picture
	const u32 fps_x1000 = src.fps_x1000 ? src.fps_x1000 : DEFAULT_FPS_X1000;
	const u32 frame_ms = 1000000 / fps_x1000;
	const u32 duration_ms = st->duration_ms ? st->duration_ms : src.duration_ms;   // for files, from the container
	bool mvd_ready = false, audio_active = false;
	u8* wb_mem = NULL;
	ndspWaveBuf wavebufs[NUM_WAVEBUFS];
	u32 wb_pts[NUM_WAVEBUFS];
	u32 mvd_workbuf = 0, decode_errors = 0;
	MVDSTD_Config cfg;

	if (vscale > 1)
		tlog("direct: %lux%lu is bigger than a texture: MVD scales it to %lux%lu", (unsigned long)stream_w,
		     (unsigned long)stream_h, (unsigned long)w, (unsigned long)h);
	// Big pictures (over 600 KB each) get a smaller pool, since a 1080p stream's MVD work
	// buffer alone takes ~18 MB of the ~27 MB linear heap.
	const int pool = (u32)w * h * 2 > BIG_PICTURE_BYTES ? BIG_PICTURE_POOL : FRAME_QUEUE;
	if (!alloc_frame_buffers(b, w, h, pool))
	{
		ytsrc_stop(&src);
		if (can_fall_back)
		{
			// e.g. a full-width side-by-side upload, wider than a 1024-px texture
			tlog("direct: %lux%lu doesn't fit a texture, using the muxed stream", (unsigned long)w, (unsigned long)h);
			return D_MUXED;
		}
		pui_message("Not enough memory", "There isn't enough memory for this video's picture size.", NULL, "Back");
		return D_STOP;
	}

	// ---- the hardware decoder, with its work buffer sized from the stream's H.264 level
	mvd_workbuf = mvd_workbuf_size(stream_w, stream_h, src.h264_level);
	tlog("direct: MVD work buffer %lu KB for level %u, linear heap free %lu KB", (unsigned long)(mvd_workbuf / 1024),
	     (unsigned)src.h264_level, (unsigned long)(mem_linear_free() / 1024));
	if (mem_linear_free() < mvd_workbuf + MVD_LINEAR_MARGIN)
	{
		ytsrc_stop(&src);
		snprintf(err, sizeof(err), "This %lux%lu video needs %lu MB more memory for the hardware decoder than is free.",
		         (unsigned long)src.crop_w, (unsigned long)src.crop_h,
		         (unsigned long)(mvd_workbuf + MVD_LINEAR_MARGIN - mem_linear_free()) / (1024 * 1024) + 1);
		pui_message("Not enough memory", err, NULL, "Back");
		return D_STOP;
	}
	const MVDSTD_OutputFormat mvd_out = MVD_OUTPUT_BGR565;
	Result mvd_result = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, mvd_out, mvd_workbuf, NULL);
	if (R_FAILED(mvd_result) &&
	    mvd_workbuf < MVD_DEFAULT_WORKBUF_SIZE)   // only ever retry larger, never below what the stream needs
	{
		mvd_workbuf = MVD_DEFAULT_WORKBUF_SIZE;
		mvd_result = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, mvd_out, mvd_workbuf, NULL);
	}
	if (R_FAILED(mvd_result))
	{
		ytsrc_stop(&src);
		snprintf(err, sizeof(err), "The New 3DS hardware video decoder could not be started (0x%08lX).",
		         (unsigned long)mvd_result);
		pui_message("Decoder error", err, NULL, "Back");
		return D_STOP;
	}
	mvd_ready = true;
	mvdstdGenerateDefaultConfig(&cfg, stream_w, stream_h, w, h, NULL, (u32*)b->frame, (u32*)b->frame);

	// ---- audio
	if (audio_ok && src.audio_ch && src.audio_rate)
	{
		wb_mem = (u8*)linearAlloc(NUM_WAVEBUFS * WAVEBUF_BYTES);
		if (wb_mem)
		{
			memset(wavebufs, 0, sizeof(wavebufs));
			for (int i = 0; i < NUM_WAVEBUFS; i++) wavebufs[i].data_vaddr = wb_mem + i * WAVEBUF_BYTES;
			ndspChnReset(AUDIO_CHANNEL);
			ndspSetOutputMode(NDSP_OUTPUT_STEREO);
			ndspChnSetInterp(AUDIO_CHANNEL, NDSP_INTERP_LINEAR);
			ndspChnSetRate(AUDIO_CHANNEL, (float)src.audio_rate);
			ndspChnSetFormat(AUDIO_CHANNEL, src.audio_ch == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
			float mix[12] = { 0 };
			mix[0] = mix[1] = 1.0f;
			ndspChnSetMix(AUDIO_CHANNEL, mix);
			audio_active = true;
		}
	}

	// ---- playback loop
	PlayerVideoFrame view = {
		.tex = NULL,
		.tex_w = b->tex_w,
		.tex_h = b->tex_h,
		.coded_h = (u16)h,
		.img_w = (u16)crop_w,
		.img_h = (u16)crop_h,
	};
	const int bytes_per_frame = src.audio_ch * 2;
	const u32 audio_pkt_ms = src.audio_rate ? YTSRC_AUDIO_FRAMES * 1000u / src.audio_rate : 93;
	u64 buf_t0 = osGetTime();   // when data for the current buffering started arriving (avsync.h)
	int wb_play = 0, wb_fill = 0, wb_submitted = 0;
	bool use_audio_clock = audio_active, buffering = true, paused = false;
	u32 clock = src.start_ms;
	u64 tick_base_time = osGetTime();
	u32 tick_base_clock = src.start_ms;
	bool first_frame = true;
	// MVD returns pictures in display order, which with B-frames lags behind decode order: in
	// I0 P200 B33 ..., P200's picture only comes out once the next reference arrives. So a
	// picture can't simply take the pts of the access unit that produced it (that shows old
	// frames again and the picture shakes). Instead each one gets the smallest pts still
	// pending, goes into a pool, and the earliest in the pool is shown next.
	PtsList pending = { .n = 0 };
	u32 q_pts[FRAME_QUEUE];
	bool q_used[FRAME_QUEUE] = { false };
	int q_count = 0;
	u32 job_pts = 0;   // pts of the access unit the worker thread is decoding
	// Smooth motion is measured on one eye (the left or top half in 3D, the whole picture in
	// 2D) and the same motion is drawn over both. Measuring the whole side-by-side picture gave
	// the two eyes different vectors, so one eye's in-between pictures ghosted while the other's
	// moved, and that rivalry looks like jitter.
	VideoLayout motion_layout = VL_2D, job_layout = VL_2D;
	bool motion_reset = false;     // the worker's motion chain restarts (the layout changed)
	MeRetime retime = { 0 };       // evens out the times around dropped repeats (motion.h)
	bool repeat_pending = false;   // a repeat was dropped since the last kept picture
	u32 repeats = 0;
	u64 fps_t0 = 0;                     // nerd stats: start of the 1 s counting window, the pictures
	u32 fps_pics = 0, fps_shown0 = 0;   // drawn in it, and the video frames shown before it
	static char stats_text[400];
	u32 kept_pts = 0;         // pts of the last picture kept for the screen...
	bool kept_last = false;   // ...and whether the last rendered picture was that one
	u32 au_seen = 0;          // access units given to MVD this session (the first few are logged)
	int back = 0;
	u32 shown = 0, late = 0;
	u64 last_ui = 0;
	PlayMode cur_mode = *mode == MODE_AUTO ? app_title_mode(title) : *mode;
	u64 last_activity = osGetTime();
	u64 last_estimate = 0;
	float disparity = 0, auto_conv = 0, auto_target = 0;
	bool have_disparity = false;
	// Layout detection in Auto mode. YouTube's streams carry no 3D metadata (no st3d box, no
	// frame-packing SEI, every format "RECTANGULAR"), so the picture has to decide. Once a
	// second, for up to 30 s, we compare the two halves both side-by-side and top-bottom. Three
	// textured frames in a row with the same answer settle it; the title is only a first guess.
	int sbs_hits = 0, tb_hits = 0, flat_hits = 0, checks = 0;
	u64 last_check = 0;
	int check_half = 0;   // 1 once the side-by-side half of a check is done
	int check_logs = 0;   // layout checks logged so far (only the first few are)
	bool sbs_pair = false, sbs_tex = false;
	bool layout_decided = *mode != MODE_AUTO;
	// Titles mislead both ways: YouTube serves some "Top & Bottom" uploads flat, and macro 3D
	// may not measure as a pair. So the picture decides, but a title that names a 3D layout
	// keeps the layout button available for a manual override.
	const bool title_layout = app_title_mode(title) != MODE_2D;
	bool can_switch = *mode != MODE_AUTO || cur_mode != MODE_2D;
	int last_slot = -1;   // queue slot of the picture on screen (its data stays until reused)
	u32 underruns = 0;
	// in-place seeks: waiting for the source to reach the new position, then buffering
	bool seeking = false;
	u64 seek_t0 = 0;
	u32 seeks = 0;
	char seek_note[16] = "";   // target time shown over the last picture until playback resumes
	char cc_label[32] = "CC Off";
	bool cc_menu = false;        // the subtitle language list is open on the bottom screen
	bool set_open = false;       // the playback settings page is open on the bottom screen
	int cc_hl = 0, cc_top = 0;   // its highlighted and first visible rows (0 = Off)
	const char* cc_names[YT_MAX_CAPTIONS];
	for (int i = 0; i < st->caption_count; i++) cc_names[i] = st->captions[i].name;
	// a file with several audio tracks lists them after the subtitles
	const int n_audio = src.n_audio > 1 ? src.n_audio : 0;
	static char audio_name_buf[YTSRC_MAX_TRACKS][96];
	const char* audio_names[YTSRC_MAX_TRACKS];
	for (int a = 0; a < n_audio; a++)
	{
		audio_track_name(&src.audio[a], a, audio_name_buf[a], sizeof(audio_name_buf[a]));
		audio_names[a] = audio_name_buf[a];
	}
	const int cc_rows = st->caption_count + 1 + n_audio;
	static char sub_text[512];   // the subtitle lines currently shown
	char last_sub[64] = "";      // for logging the first few changes
	u32 sub_logs = 0;
	const u64 session_start = osGetTime();
	const bool is_youtube = strcmp(g_video_id, "file") != 0;   // comments and votes are YouTube's
	if (st->is_360)
	{
		// 360 and VR180 videos (YouTube does mark these) are shown flat, with no 3D layouts
		cur_mode = MODE_2D;
		can_switch = false;
		layout_decided = true;
	}
	else if (st->stereo != YT_STEREO_NONE && !use_muxed && *mode == MODE_AUTO)
	{
		// YouTube states the layout (an upload marked as 3D, via the VISIONOS streams), so
		// there's nothing to guess. The muxed stream is a flat one-eye version, so this only
		// applies to DASH.
		cur_mode = st->stereo == YT_STEREO_SBS ? MODE_SBS : MODE_TB;
		*mode = cur_mode;
		can_switch = true;
		layout_decided = true;
		tlog("direct: YouTube marks this video 3D: %s", play_mode_name(cur_mode));
	}
	// pacing: when the top screen last refreshed (C3D counts the VBlank0 events)
	u32 vb_at_present = 0;          // C3D_FrameCounter(0) when the last frame was drawn
	SmoothClock smoothed = { 0 };   // see avsync.h
	u64 vb_tick = svcGetSystemTick();
	u32 vb_seen = C3D_FrameCounter(0);                                // the last refresh the loop woke up for
	const u32 cadence = (u32)((float)frame_ms / REFRESH_MS + 0.5f);   // refreshes per video frame
	// Cadence lock: each frame stays up for hold_lo..hold_hi refreshes (2 at 30 fps, 2 or 3 at
	// 24/25 fps, 1 at 60). Deciding by the clock alone makes frames whose time falls near a
	// refresh boundary flip back and forth: on hardware about 5% of a 30 fps video's frames
	// were held for 1 or 3 refreshes, a micro-stutter every second. With the lock, only real
	// drift of a whole refresh breaks the rhythm, such as the 60 vs 59.83 Hz correction about
	// every 6 s.
	const float per_frame = 1000000.0f / (float)fps_x1000 / REFRESH_MS;
	const u32 hold_lo = (u32)floorf(per_frame + 0.05f), hold_hi = (u32)ceilf(per_frame - 0.05f);
	// Smooth motion (Settings, see app.h and motion.h): for videos slower than the screen we
	// draw an in-between picture at every refresh, following the motion between two decoded
	// pictures. It switches itself off again if it can't keep up.
	bool smooth_motion = app.motion != MOTION_NORMAL && per_frame > 1.2f && vmesh_ready();
	static u8 sm_luma[FRAME_QUEUE][ME_MAX_W * ME_MAX_H];   // luma maps of the pooled pictures
	static bool sm_luma_ok[FRAME_QUEUE];
	static MotionField sm_fld[FRAME_QUEUE];   // motion from a pooled picture to the one after it
	static bool sm_fld_ok[FRAME_QUEUE];
	static u32 sm_fld_to[FRAME_QUEUE];       // ...and that next picture's pts
	static u8 sm_cur[ME_MAX_W * ME_MAX_H];   // luma map of the picture on screen
	static MotionField sm_field, sm_prev;    // motion from the picture on screen to the next, and the previous one
	static float sm_vx[(ME_MAX_BW + 1) * (ME_MAX_BH + 1)], sm_vy[(ME_MAX_BW + 1) * (ME_MAX_BH + 1)];
	int sm_lw = 0, sm_lh = 0;
	bool sm_have_cur = false;   // a picture is on screen under Smooth motion (sm_cur_pts is its time)
	bool sm_cur_ok = false, sm_field_ok = false, sm_prev_ok = false;
	u32 sm_cur_pts = 0, sm_field_pts = 0;
	int blend_slot = -1;   // queue slot already copied into the spare texture (the next picture)
	u32 interps = 0, cuts = 0,
	    late_fields = 0;   // in-between pictures, scene cuts, refreshes whose motion wasn't ready
	u32 sm_idle = 0, sm_missed = 0,
	    sm_last_vb = 0;   // refreshes that kept the old picture (jitter), and missed refreshes
	double sm_disp = 0;   // display clock: the media time at the middle of the coming refresh
	bool sm_disp_ok = false;
	u64 me_ticks = 0, me_max = 0, luma_ticks = 0;
	u32 me_n = 0, luma_n = 0;
	// timing stats for the log
	u64 dec_ticks = 0, dec_max = 0, draw_ticks = 0, draw_max = 0;
	u32 dec_n = 0, uneven = 0;
	u32 skipped_late = 0;      // unreferenced pictures left out because MVD was behind
	u32 held[5] = { 0 };       // how many refreshes each frame stayed on screen: 1, 2, 3, 4, 5+
	bool at_refresh = false;   // the loop just woke up on a top-screen refresh
	// loop stalls: anything that holds the loop up long enough to cost frames
	u64 loop_tick = 0, stall_max = 0;
	u32 stalls = 0, skipped = 0;
	// Logged every 5 s of playback: frames shown and dropped, and how fast the media clock runs
	// against the CPU clock. 1.000 is real time; above that the video falls behind and drops frames.
	u64 win_tick = 0;
	u64 play_t0 = 0;                // when this session first left buffering (0 if it hasn't yet)
	bool win_after_start = false;   // this window began as playback (re)started, with the decoder warming up
	u32 win_clock = 0, win_shown = 0, win_late = 0, late_logged = 0, win_interps = 0, win_repeats = 0;
	tlog("direct: threads on core %d (demux/decode) and %d (download); 2 = the New 3DS's extra core", src.source_core,
	     src.net_core);
	tlog("direct: %lux%lu (picture %lux%lu) %lu.%03lu fps, %s, from %lu ms, MVD %lu KB, audio %s", (unsigned long)w,
	     (unsigned long)h, (unsigned long)src.crop_w, (unsigned long)src.crop_h, (unsigned long)(fps_x1000 / 1000),
	     (unsigned long)(fps_x1000 % 1000), dash ? "DASH" : "muxed", (unsigned long)*pos_ms,
	     (unsigned long)(mvd_workbuf / 1024), audio_active ? "on" : "off");

	playback_home_block(true);   // we handle HOME ourselves below, once the decoder is shut down
	const bool worker_ok = mvd_worker_start();
	if (!worker_ok) tlog("direct: could not start the decoder thread");
	while (worker_ok)
	{
		if (!aptMainLoop())
		{
			exit_requested("direct: playing");
			action = D_STOP;
			*pos_ms = clock;
			break;
		}
		{
			u64 now_tick = svcGetSystemTick();
			if (loop_tick && !paused)
			{
				u64 gap = now_tick - loop_tick;
				if (gap > STALL_MS * (u64)CPU_TICKS_PER_MSEC) stalls++;
				if (gap > stall_max) stall_max = gap;
			}
			loop_tick = now_tick;
		}
		if (pui_gpu_dead())
		{
			// The GPU has stopped (a draw or copy never finished), so nothing more can be drawn.
			ndspChnWaveBufClear(AUDIO_CHANNEL);
			exit_through_apt();   // going back to HOME beats a frozen app
		}
		s64 seek_to = -1;
		if (playback_home_pressed())
		{
			// The decoder and audio stop (the state HOME is known to work in on hardware), but
			// the download keeps its connections. Coming back is an in-place seek to this point
			// with the last picture still up, rather than a new session behind the loading screen.
			u32 at = clock;
			if (audio_active) ndspChnSetPaused(AUDIO_CHANNEL, true);
			mvd_worker_wait_idle();
			if (mvd_ready)
			{
				mvdstdExit();
				mvd_ready = false;
			}
			playback_go_home("direct");
			if (aptShouldClose())
			{
				action = D_STOP;
				*pos_ms = at;
				break;
			}
			playback_home_block(true);
			// the picture's texture lives in VRAM, which the HOME menu may have used, so copy it again
			if (last_slot >= 0 && view.tex) show_frame(b, last_slot, (C3D_Tex*)view.tex, w, h);
			power_bottom_backlight(true);
			last_activity = osGetTime();
			if (st->is_live)
			{
				// a live stream can't go back to where it was, so reconnect at the live edge
				tlog("direct: back from HOME: live, reconnecting at the edge");
				action = D_SEEK;
				*pos_ms = 0;
				break;
			}
			tlog("direct: back from HOME, resuming at %lu ms in place", (unsigned long)at);
			seek_to = at;
		}
		hidScanInput();
		u32 keys = hidKeysDown();
		s64 touch_seek = -1;
		if (!power_bottom_backlight_on() && (keys || (hidKeysHeld() & KEY_TOUCH)))
		{
			// the first press only wakes the bottom screen, so nothing is hit by accident
			power_bottom_backlight(true);
			keys = 0;
			last_activity = osGetTime();
		}
		else
		{
			keys |= pui_touch(duration_ms, &touch_seek);
			if (keys || touch_seek >= 0 || (hidKeysDown() & KEY_TOUCH) || pui_scrubbing())
			{
				last_activity = osGetTime();
				// The remote only redraws 5 times a second while playing. Redraw right away after
				// a press, or it feels unresponsive and people press again.
				last_ui = 0;
			}
		}
		// To save battery the bottom screen goes dark after a while without input, but not while
		// paused or buffering, when you're likely to be looking at it.
		if (!paused && !buffering && osGetTime() - last_activity > BACKLIGHT_IDLE_MS) power_bottom_backlight(false);
		else if (paused || buffering) power_bottom_backlight(true);
		// ---- the comments (SELECT or the channel row). Holding Up/Down or dragging the stylus
		// scrolls them; B, SELECT or Close shuts them. The video keeps playing and A still pauses.
		if (g_cm_open)
		{
			const u32 cm_held = hidKeysHeld();
			if (cm_held & KEY_DUP) pui_comments_scroll(-5);
			if (cm_held & KEY_DDOWN) pui_comments_scroll(5);
			if (keys & (KEY_B | KEY_SELECT)) g_cm_open = false;
			if (pui_comments_want_more()) comments_fetch();
			if (keys || (cm_held & (KEY_DUP | KEY_DDOWN)) || pui_comments_active()) last_ui = 0;
			keys &= ~(u32)(KEY_B | KEY_SELECT | KEY_X | KEY_Y | KEY_DUP | KEY_DDOWN);
		}
		else if ((keys & KEY_SELECT) && is_youtube && !cc_menu)
		{
			g_cm_open = true;
			if (!g_cm_started) comments_fetch();
			last_ui = 0;
			keys &= ~(u32)KEY_SELECT;
		}
		{
			static bool was_busy = false;   // when a page arrives, show it at once
			if (was_busy && !g_cm_busy) last_ui = 0;
			was_busy = g_cm_busy;
		}
		// ---- the playback settings page (START or the Settings button). The video keeps playing;
		// its rows do what their buttons do (Y, X, Up/Down), so those keys still work here too.
		if (set_open)
		{
			const int tap = pui_settings_tapped();
			if (tap == PUI_SET_CC)
			{
				set_open = false;
				keys |= KEY_Y;
			}   // the list below opens
			else if (tap == PUI_SET_LAYOUT) keys |= KEY_X;
			else if (tap == PUI_SET_STATS)
			{
				app.nerd_stats = !app.nerd_stats;
				app.settings_changed = true;
			}
			else if (tap == PUI_SET_SMOOTH)
			{
				app.motion = app.motion == MOTION_NORMAL ? MOTION_SMOOTH : MOTION_NORMAL;
				app.settings_changed = true;
				// from the next pictures on: with Smooth back on, its motion data starts fresh
				smooth_motion = app.motion != MOTION_NORMAL && per_frame > 1.2f && vmesh_ready();
				sm_have_cur = sm_cur_ok = sm_field_ok = sm_prev_ok = false;
				for (int i = 0; i < FRAME_QUEUE; i++) sm_luma_ok[i] = sm_fld_ok[i] = false;
				tlog("direct: smooth motion %s from the settings page", smooth_motion ? "on" : "off");
			}
			else if (tap >= PUI_SET_DEPTH)
			{
				g_depth_step = (s8)(tap - PUI_SET_DEPTH - PUI_DEPTH_STEPS);
				tlog("direct: depth %+d", g_depth_step);
			}
			if (keys & (KEY_B | KEY_START)) set_open = false;
			if (keys || tap != PUI_SET_NONE) last_ui = 0;
			keys &= ~(u32)(KEY_B | KEY_START | KEY_SELECT);
		}
		else if ((keys & KEY_START) && !g_cm_open && !cc_menu)
		{
			set_open = true;
			last_ui = 0;
			keys &= ~(u32)KEY_START;
		}
		// ---- the subtitle language list (Y or the CC button). The video keeps playing underneath;
		// while the list is open, Up/Down/A/B work the list instead of changing depth, pausing or
		// going back.
		if (cc_menu)
		{
			int pick = pui_cc_tapped();
			if ((keys & KEY_DUP) && cc_hl > 0) cc_hl--;
			if ((keys & KEY_DDOWN) && cc_hl + 1 < cc_rows) cc_hl++;
			if (keys & KEY_A) pick = cc_hl;
			if (pick >= 0 && pick <= st->caption_count) sub_choose(subs, st, pick - 1);
			else if (pick > st->caption_count && pick < cc_rows)
			{
				const int track = pick - st->caption_count - 1;
				if (track != src.audio_cur)
				{
					// Another audio track means a new session from this point, since the sound
					// format may change.
					tlog("direct: audio track -> %d (stream %d) at %lu ms", track, src.audio[track].stream,
					     (unsigned long)clock);
					*audio_stream = src.audio[track].stream;
					action = D_AUDIO;
					*pos_ms = clock;
					break;
				}
			}
			if (pick >= 0 || (keys & (KEY_B | KEY_Y))) cc_menu = false;
			if (cc_hl < cc_top) cc_top = cc_hl;
			if (cc_hl >= cc_top + PUI_CC_ROWS) cc_top = cc_hl - PUI_CC_ROWS + 1;
			if (keys || pick >= 0) last_ui = 0;
			keys &= ~(u32)(KEY_A | KEY_B | KEY_X | KEY_Y | KEY_DUP | KEY_DDOWN);
		}
		else if ((keys & KEY_Y) && (st->caption_count || n_audio))
		{
			cc_menu = true;
			cc_hl = st->caption_count || !n_audio ? subs->want + 1 : st->caption_count + 1 + src.audio_cur;
			cc_top = cc_hl >= PUI_CC_ROWS ? cc_hl - PUI_CC_ROWS + 1 : 0;
			last_ui = 0;
			keys &= ~(u32)KEY_Y;
		}

		if (touch_seek >= 0)
		{
			tlog("direct: seek bar tapped: %lu -> %lu ms", (unsigned long)clock, (unsigned long)touch_seek);
			seek_to = touch_seek;
		}
		if (keys & KEY_B)
		{
			tlog("direct: Back at %lu ms", (unsigned long)clock);
			action = D_STOP;
			*pos_ms = clock;
			break;
		}
		if (keys & KEY_A)
		{
			paused = !paused;
			tlog("direct: %s at %lu ms", paused ? "paused" : "resumed", (unsigned long)clock);
			if (audio_active) ndspChnSetPaused(AUDIO_CHANNEL, paused);
			if (!use_audio_clock)
			{
				if (paused) tick_base_clock = clock;
				else tick_base_time = osGetTime();
			}
		}
		s32 seek = 0;
		if (keys & KEY_DLEFT) seek = -SEEK_SHORT_MS;
		if (keys & KEY_DRIGHT) seek = SEEK_SHORT_MS;
		if (keys & KEY_L) seek = -SEEK_LONG_MS;
		if (keys & KEY_R) seek = SEEK_LONG_MS;
		if (st->is_live) seek = 0;   // live streams always play at the edge
		if (seek)
		{
			s64 target = (s64)clock + seek;
			if (target < 0) target = 0;
			if (duration_ms && target > (s64)duration_ms - SEEK_END_GAP_MS)
				target = duration_ms > SEEK_END_GAP_MS ? duration_ms - SEEK_END_GAP_MS : 0;
			tlog("direct: seek %+ld s: %lu -> %lu ms", (long)(seek / 1000), (unsigned long)clock,
			     (unsigned long)target);
			seek_to = target;
		}
		if (seek_to >= 0 && ytsrc_has_failed(&src))
		{
			// the source is gone (a network error), so start a new session at the target
			action = D_SEEK;
			*pos_ms = (u32)seek_to;
			break;
		}
		if (seek_to >= 0)
		{
			// Seek in place: the source seeks inside its open streams without reconnecting or
			// reading a new index, and the last picture stays up with a spinner until the new
			// position plays. MVD and the audio channel restart exactly as at a session start.
			ytsrc_seek(&src, (u32)seek_to);
			peek_cur(&video_ring, ytsrc_gen_tag(&src));   // drop everything queued for the old position
			peek_cur(&audio_ring, ytsrc_gen_tag(&src));
			for (int i = 0; i < FRAME_QUEUE; i++) q_used[i] = false;
			blend_slot = -1;
			sm_have_cur = sm_cur_ok = sm_field_ok = sm_prev_ok =
			    false;   // Smooth motion starts again at the new position
			sm_last_vb = 0;
			sm_disp_ok = false;
			q_count = 0;
			pending.n = 0;
			if (audio_active)
			{
				ndspChnWaveBufClear(AUDIO_CHANNEL);
				ndspChnSetPaused(AUDIO_CHANNEL, false);
				wb_play = wb_fill = wb_submitted = 0;
			}
			mvd_worker_wait_idle();   // let a job in flight finish first; its picture is thrown away
			wk.busy = false;
			wk.prev_ok = wk.prev_field_ok = false;
			wk.rep = (MeRepeat){ 0 };
			wk.cfg_buf = -1;
			retime = (MeRetime){ 0 };
			repeat_pending = false;
			kept_last = false;
			if (mvd_ready) mvdstdExit();   // never twice, because libctru reference-counts Init/Exit
			mvd_result = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, mvd_out, mvd_workbuf, NULL);
			mvd_ready = R_SUCCEEDED(mvd_result);
			if (R_FAILED(mvd_result))
			{
				tlog("direct: decoder restart failed (0x%08lX), restarting the session", (unsigned long)mvd_result);
				action = D_SEEK;
				*pos_ms = (u32)seek_to;
				break;
			}
			first_frame = true;
			paused = false;
			buffering = true;
			seeking = true;
			seek_t0 = osGetTime();
			seeks++;
			use_audio_clock = audio_active;
			clock = (u32)seek_to;
			smooth_clock_reset(&smoothed);
			vb_at_present = 0;   // the next frame's hold time means nothing
			win_tick = 0;
			fmt_time(seek_note, sizeof(seek_note), clock);
			last_ui = 0;
			continue;
		}
		if (keys & (KEY_DUP | KEY_DDOWN))
		{
			if ((keys & KEY_DUP) && g_depth_step < DEPTH_STEPS) g_depth_step++;
			if ((keys & KEY_DDOWN) && g_depth_step > -DEPTH_STEPS) g_depth_step--;
			tlog("direct: depth %+d", g_depth_step);
			last_ui = 0;   // show the new value right away
		}
		if ((keys & KEY_X) && can_switch)
		{
			// We already have the picture, so switching the 3D layout needs no reload. The
			// user's choice beats the layout detector, which stops here.
			cur_mode = next_mode(cur_mode);
			tlog("direct: layout switched to %s", play_mode_name(cur_mode));
			*mode = cur_mode;
			layout_decided = true;
			auto_target = auto_conv = 0;
			have_disparity = false;
		}
		view.layout = (cur_mode == MODE_SBS || cur_mode == MODE_SBS_SWAP) ? VL_SBS
		              : (cur_mode == MODE_TB || cur_mode == MODE_TB_SWAP) ? VL_TB
		                                                                  : VL_2D;
		view.swap_eyes = cur_mode == MODE_SBS_SWAP || cur_mode == MODE_TB_SWAP;
		float ratio = (float)src.crop_w / (float)src.crop_h;
		view.full = view.layout == VL_SBS ? ratio >= 3.0f : view.layout == VL_TB ? ratio <= 1.2f : false;
		if (view.layout != motion_layout)
		{
			// the eye region changed, so luma maps and motion measured on the old one no longer fit
			motion_layout = view.layout;
			motion_reset = true;
			for (int i = 0; i < FRAME_QUEUE; i++) sm_luma_ok[i] = sm_fld_ok[i] = false;
			sm_cur_ok = sm_field_ok = sm_prev_ok = false;
		}

		// ---- audio: reclaim finished buffers and queue new ones
		if (audio_active)
		{
			while (wb_submitted > 0 && *(volatile u8*)&wavebufs[wb_play].status == NDSP_WBUF_DONE)
			{
				wb_play = (wb_play + 1) % NUM_WAVEBUFS;
				wb_submitted--;
			}
			if (!buffering && !paused)
				while (wb_submitted < NUM_WAVEBUFS)
				{
					Pkt* p = peek_cur(&audio_ring, ytsrc_gen_tag(&src));
					if (!p) break;
					u32 len = p->len > WAVEBUF_BYTES ? WAVEBUF_BYTES : p->len;
					ndspWaveBuf* wbuf = &wavebufs[wb_fill];
					memcpy((void*)wbuf->data_vaddr, p->data, len);
					wbuf->nsamples = len / bytes_per_frame;
					wbuf->looping = false;
					wbuf->status = NDSP_WBUF_FREE;
					wb_pts[wb_fill] = p->pts;
					ring_release(&audio_ring);
					if (!wbuf->nsamples) continue;
					DSP_FlushDataCache(wbuf->data_vaddr, len);
					ndspChnWaveBufAdd(AUDIO_CHANNEL, wbuf);
					wb_fill = (wb_fill + 1) % NUM_WAVEBUFS;
					wb_submitted++;
				}
		}

		// ---- master clock
		if (!buffering && !paused)
		{
			if (use_audio_clock)
			{
				if (wb_submitted > 0)
				{
					ndspWaveBuf* wbuf = &wavebufs[wb_play];
					u32 pos =
					    (*(volatile u8*)&wbuf->status == NDSP_WBUF_PLAYING) ? ndspChnGetSamplePos(AUDIO_CHANNEL) : 0;
					clock = wb_pts[wb_play] + (u32)((u64)pos * 1000 / src.audio_rate);
				}
				else if ((ytsrc_is_eof(&src) || ytsrc_has_failed(&src)) && ring_count(&audio_ring) == 0)
				{
					use_audio_clock = false;
					tick_base_clock = clock;
					tick_base_time = osGetTime();
				}
				else
				{
					buffering = true;   // the audio ran dry, so wait for the network
					underruns++;
					buf_t0 = osGetTime();
					tlog("direct: buffering at %lu ms (audio ran out; %lu video packets queued)", (unsigned long)clock,
					     (unsigned long)ring_count(&video_ring));
				}
			}
			else if (ring_count(&video_ring) == 0 && q_count == 0 && !ytsrc_is_eof(&src) && !ytsrc_has_failed(&src))
			{
				// There's no audio clock (a silent video, no DSP firmware, or the audio ended)
				// and the video ran out, so wait for the network instead of letting the clock run on.
				buffering = true;
				underruns++;
				buf_t0 = osGetTime();
				tlog("direct: buffering at %lu ms (video ran out)", (unsigned long)clock);
			}
			else clock = tick_base_clock + (u32)(osGetTime() - tick_base_time);

			if (!buffering) clock = smooth_clock(&smoothed, clock, (double)svcGetSystemTick() / CPU_TICKS_PER_MSEC);
		}
		else smooth_clock_reset(&smoothed);

		// ---- video. The MVD worker thread does the decoding (see mvd_worker_main). This loop
		// only hands it access units and collects the finished pictures, so it never blocks on
		// the decoder and every refresh gets its picture. First take a finished job, then post
		// the next one.
		if (wk.busy && wk.done)
		{
			__sync_synchronize();   // read the thread's results only after its done flag
			wk.busy = false;
			if (job_layout != motion_layout)   // these were measured on the old eye region
				for (int o = 0; o < wk.n_out; o++) wk.out[o].luma_ok = wk.out[o].field_ok = wk.out[o].repeat = false;
			Result mvd_status = wk.r;
			if (MVD_CHECKNALUPROC_SUCCESS(mvd_status)) pts_add(&pending, job_pts);
			else decode_errors++;
			// log how MVD releases the first pictures of a session (reordering delay, bursts)
			if (au_seen++ < 40)
				tlog("direct: mvd au %lu pts %lu -> status 0x%lX, %d pictures (%lu in processing, %lu by render, %lu "
				     "renders), %d pts pending, %d queued",
				     (unsigned long)au_seen, (unsigned long)job_pts, (unsigned long)mvd_status, wk.n_out,
				     (unsigned long)wk.written_in_process, (unsigned long)wk.written_by_render,
				     (unsigned long)wk.renders, pending.n, q_count);
			dec_ticks += wk.dec_ticks;
			if (wk.dec_ticks > dec_max) dec_max = wk.dec_ticks;
			dec_n++;
			// the 3D analysis the worker did on this job's first picture
			if (wk.m_done && wk.measure == MEASURE_CONV && wk.measure_layout == view.layout)
			{
				// dark or flat frames, or halves that aren't the same scene, give meaningless shifts
				if (wk.m_tex && wk.m_pair)
				{
					const float measured = wk.m_disp;
					disparity = have_disparity ? disparity * 0.5f + measured * 0.5f : measured;
					have_disparity = true;
					const float scale = screen_scale(view.layout, crop_w);
					float screen_disp = disparity * scale;
					auto_target = screen_disp > MAX_BEHIND_PX   ? (screen_disp - MAX_BEHIND_PX) / scale
					              : screen_disp < -MAX_FRONT_PX ? (screen_disp + MAX_FRONT_PX) / scale
					                                            : 0.0f;
				}
			}
			// In Auto mode, work out which layout the frames really have (see layout_decided). Each
			// check measures the side-by-side split on one job's picture and the top-bottom split
			// on the next.
			else if (wk.m_done && wk.measure == MEASURE_SBS && !layout_decided)
			{
				sbs_pair = wk.m_pair;
				sbs_tex = wk.m_tex;
				check_half = 1;
			}
			else if (wk.m_done && wk.measure == MEASURE_TB && !layout_decided && check_half == 1)
			{
				last_check = osGetTime();
				check_half = 0;
				const bool tb_pair = wk.m_pair, tb_tex = wk.m_tex;
				if (check_logs++ < 12)
					tlog("direct: layout check at %lu ms: side-by-side %s%s, top-bottom %s%s", (unsigned long)clock,
					     sbs_pair ? "pair" : "no pair", sbs_tex ? "" : " (flat)", tb_pair ? "pair" : "no pair",
					     tb_tex ? "" : " (flat)");
				if (sbs_tex && tb_tex)   // dark or flat frames can't tell us anything
				{
					checks++;
					// One layout has to match and the other not. A plain picture, like sky or a
					// wall, looks alike in both splits and proves nothing.
					sbs_hits = sbs_pair && !tb_pair ? sbs_hits + 1 : 0;
					tb_hits = tb_pair && !sbs_pair ? tb_hits + 1 : 0;
					flat_hits = !sbs_pair && !tb_pair ? flat_hits + 1 : 0;
					// When the title names a 3D layout, flat checks never switch to 2D, only
					// to the other layout. SBS trailers often open with several seconds that
					// measure flat, and flipping to 2D and back looks worse than the occasional
					// mislabelled upload, which the layout button can fix.
					PlayMode found = sbs_hits >= LAYOUT_HITS                     ? MODE_SBS
					                 : tb_hits >= LAYOUT_HITS                    ? MODE_TB
					                 : flat_hits == LAYOUT_HITS && !title_layout ? MODE_2D
					                                                             : MODE_AUTO;
					if (found != MODE_AUTO)
					{
						// if the layout is the same, keep "eyes swapped" from a cross-eyed title
						bool same = (found == MODE_SBS && (cur_mode == MODE_SBS || cur_mode == MODE_SBS_SWAP)) ||
						            (found == MODE_TB && (cur_mode == MODE_TB || cur_mode == MODE_TB_SWAP)) ||
						            found == cur_mode;
						tlog("direct: frames are %s (title guessed %s)%s", play_mode_name(found),
						     play_mode_name(cur_mode), same ? "" : ", switching");
						if (!same)
						{
							cur_mode = found;
							auto_target = auto_conv = 0;
							have_disparity = false;
						}
						*mode = cur_mode;
						can_switch = cur_mode != MODE_2D;
						layout_decided = true;
					}
				}
				if (checks >= LAYOUT_MAX_CHECKS || osGetTime() - session_start > LAYOUT_MAX_MS) layout_decided = true;
			}
			for (int oi = 0; oi < wk.n_out; oi++)
			{
				const WkOut* pic = &wk.out[oi];
				b->frame = b->outs[pic->buf];   // this picture
				if (q_count >= pool || pending.n == 0)
				{
					// no room or no time for it: it's never shown, but its pts still has to go
					if (pending.n) pts_take(&pending);
					late++;
					continue;
				}
				int slot = 0;
				while (q_used[slot] || !b->tiled[slot]) slot++;   // there is a free slot, since q_count < pool
				queue_frame(b, slot, w, h);
				bool keep = true;
				u32 out_pts = pts_take(&pending);
				if (buffering && use_audio_clock)
				{
					Pkt* first_audio = peek_cur(&audio_ring, ytsrc_gen_tag(&src));
					if (first_audio && out_pts + frame_ms < first_audio->pts)
					{
						keep = false;   // it's before the audio starts, so it would never be shown
						skipped++;
					}
				}
				const bool repeat = keep && pic->luma_ok && pic->repeat;
				if (repeat)
				{
					keep = false;   // the frames on either side are stretched over its time
					repeats++;
					repeat_pending = true;
				}
				if (keep && pic->luma_ok)
				{
					// while repeats are being dropped, the kept frames are re-timed evenly (me_retime)
					out_pts = (u32)(me_retime(&retime, (double)out_pts, repeat_pending) + 0.5);
					repeat_pending = false;
				}
				if (keep)
				{
					q_pts[slot] = out_pts;
					q_used[slot] = true;
					q_count++;
					// For Smooth motion the thread built this picture's luma map and the
					// motion from the picture before it. That motion is attached to the
					// earlier picture (in the pool, or the one on screen), as long as it
					// was kept and is still around.
					sm_luma_ok[slot] = pic->luma_ok;
					sm_fld_ok[slot] = false;
					if (pic->luma_ok)
					{
						sm_lw = wk.lw;
						sm_lh = wk.lh;
						memcpy(sm_luma[slot], wk_luma[oi], (size_t)sm_lw * sm_lh);
					}
					if (pic->field_ok && kept_last)
					{
						int from = -1;
						for (int i = 0; i < FRAME_QUEUE; i++)
							if (q_used[i] && i != slot && q_pts[i] == kept_pts) from = i;
						if (from >= 0)
						{
							sm_fld[from] = wk_field[oi];
							sm_fld_to[from] = out_pts;
							sm_fld_ok[from] = true;
						}
						else if (sm_have_cur && sm_cur_pts == kept_pts)
						{
							sm_field = wk_field[oi];
							sm_field_pts = out_pts;
							sm_field_ok = true;
							me_vertex_vectors(&sm_field, sm_vx, sm_vy);
							sm_prev = sm_field;
							sm_prev_ok = true;
						}
					}
					kept_pts = out_pts;
				}
				if (!repeat) kept_last = keep;   // a dropped repeat leaves the motion chain alone
				if (pic->luma_ok) luma_n++;
				if (pic->field_ok)
				{
					me_n++;
					if (!wk_field[oi].usable) cuts++;
				}
			}
			luma_ticks += wk.luma_ticks;
			me_ticks += wk.me_ticks;
			if (wk.me_ticks > me_max) me_max = wk.me_ticks;
		}
		if (!wk.busy && q_count <= pool - MVD_OUTS)   // room for everything one access unit can give
		{
			Pkt* p = peek_cur(&video_ring, ytsrc_gen_tag(&src));
			// When MVD falls behind. B frames cost the hardware about twice a P frame, and a
			// 1080p stream with them averages 56 ms a frame, while 24 fps needs 41.7 ms (without
			// B frames it's 32 ms). So a picture that nothing else refers to is skipped when it
			// would reach the screen late anyway. That's safe because nothing depends on it, the
			// other pictures stay on time, and Smooth draws its moment from the motion between
			// its neighbours.
			if (p && (p->pad & YTSRC_DISPOSABLE) && !buffering && !first_frame && dec_n >= 8)
			{
				const float avg_dec = (float)dec_ticks / dec_n / CPU_TICKS_PER_MSEC;
				// Keep MVD 0.4 s ahead of the clock. With that lead the next picture is ready in
				// time and Smooth fills the refreshes in between. A 250 ms lead sometimes left the
				// pool empty, and the motion got about ten times less even.
				int lead = 400;   // ms
				if (avg_dec > frame_ms * 0.8f &&
				    (s32)(p->pts - clock) < (s32)(avg_dec * 1.5f) + lead)   // only for a slow stream
				{
					ring_release(&video_ring);
					skipped_late++;
					p = NULL;
				}
			}
			if (p && p->len + 3 <= NAL_BUF_SIZE)
			{
				memcpy(b->nal, p->data, p->len);
				GSPGPU_FlushDataCache(b->nal, p->len);
				wk.nal = b->nal;
				wk.len = p->len;
				wk.first = first_frame;
				wk.cfg = &cfg;
				for (int i = 0; i < MVD_OUTS; i++) wk.outs[i] = b->outs[i];
				wk.frame_bytes = b->frame_bytes;
				wk.coded_w = w;
				wk.want_motion = smooth_motion;
				wk.measure = MEASURE_NONE;
				if (view.layout != VL_2D && osGetTime() - last_estimate >= MEASURE_EVERY_MS)
				{
					wk.measure = MEASURE_CONV;
					last_estimate = osGetTime();
				}
				else if (!layout_decided && !st->is_360 &&
				         (check_half == 1 || osGetTime() - last_check >= MEASURE_EVERY_MS))
					wk.measure = check_half == 0 ? MEASURE_SBS : MEASURE_TB;
				wk.measure_layout = view.layout;
				wk.measure_swap = view.swap_eyes;
				wk.crop_w = crop_w;
				wk.crop_h = crop_h;
				// Pulldown repeats only exist where a film was converted to 30 or 60 fps. Skipping the
				// check for 24/25 fps uploads means they never lose a real frame that merely looks
				// alike, as happens in dark, slow scenes.
				wk.want_repeats = fps_x1000 >= 29000;
				wk.stride = (int)w;
				wk.pic_w = (int)(motion_layout == VL_SBS ? crop_w / 2 : crop_w);   // the left or top eye
				wk.pic_h = (int)(motion_layout == VL_TB ? crop_h / 2 : crop_h);
				if (motion_reset)
				{
					wk.prev_ok = wk.prev_field_ok = false;
					wk.rep = (MeRepeat){ 0 };
				}
				motion_reset = false;
				job_layout = motion_layout;
				job_pts = p->pts;
				first_frame = false;
				ring_release(&video_ring);
				wk.done = false;
				wk.busy = true;
				__sync_synchronize();   // publish the job before waking the thread
				LightEvent_Signal(&wk.go);
			}
			else if (p)
			{
				decode_errors++;   // an absurdly large access unit
				ring_release(&video_ring);
			}
		}

		// ---- an in-place seek has reached its new position, so buffer from there
		if (seeking && ytsrc_seek_done(&src))
		{
			seeking = false;
			clock = src.start_ms;   // the keyframe at or before the target
			buf_t0 = osGetTime();
			tlog("direct: seek in place: at %lu ms after %lu ms", (unsigned long)clock,
			     (unsigned long)(buf_t0 - seek_t0));
		}

		// ---- leave buffering once enough is queued (a full ring counts as enough). How much is
		// enough depends on how fast the download is filling it (avsync.h).
		if (buffering && !seeking)
		{
			u32 v_ms = (u32)(ring_count(&video_ring) + q_count) * frame_ms;
			u32 a_ms = (u32)ring_count(&audio_ring) * audio_pkt_ms;
			u32 queued = use_audio_clock && a_ms < v_ms ? a_ms : v_ms;
			u32 waited = (u32)(osGetTime() - buf_t0);
			u32 want = buffer_want_ms(queued, waited, underruns, play_t0 == 0);
			BufferState buf_state = {
				.video_count = ring_count(&video_ring) + q_count,
				.audio_count = ring_count(&audio_ring),
				.video_used = ring_used(&video_ring),
				.video_cap = VRING_BYTES,
				.audio_used = ring_used(&audio_ring),
				.audio_cap = ARING_BYTES,
				.prebuffer = (int)(want / frame_ms) > 1 ? (int)(want / frame_ms) : 1,
				.audio_prebuffer = (int)((want + audio_pkt_ms - 1) / audio_pkt_ms),
				.need_audio = use_audio_clock,
				.stream_done = ytsrc_is_eof(&src) || ytsrc_has_failed(&src),
				.video_blocked = video_ring.full,
			};
			// Also wait for a few decoded pictures. Starting with an empty pool made the first
			// pictures arrive 100-160 ms late, and the first second stuttered. We don't wait more
			// than 1 s beyond what the buffer wanted, or at the end of the stream.
			const bool pictures_ready = q_count >= 3 || buf_state.stream_done || waited > want + 1000;
			if (buffering_done(&buf_state) && pictures_ready)
			{
				buffering = false;
				seek_note[0] = 0;
				if (!play_t0) play_t0 = osGetTime();
				tlog("direct: playing at %lu ms (wanted %lu ms, %lu ms queued in %lu ms; %lu video, %lu audio packets)",
				     (unsigned long)clock, (unsigned long)want, (unsigned long)queued, (unsigned long)waited,
				     (unsigned long)ring_count(&video_ring), (unsigned long)ring_count(&audio_ring));
				if (!use_audio_clock)
				{
					tick_base_clock = clock;
					tick_base_time = osGetTime();
				}
			}
		}

		// ---- present a frame when it's due; frames far behind are dropped so the picture catches up
		bool drew = false;
		// The remote is only redrawn while the bottom screen is lit, 5 times a second while
		// playing, and in its own bottom-only GPU frame. Drawing it together with a video frame
		// makes that frame miss its refresh.
		bool ui_due = power_bottom_backlight_on() &&
		              osGetTime() - last_ui >= (buffering || paused || cc_menu || g_cm_open ? 100 : UI_PLAY_MS);
		PlayerStatus status = { 0 };
		if (ui_due)
		{
			status = (PlayerStatus){
				.title = st->title[0] ? st->title : title,
				.pos_ms = clock,
				.dur_ms = duration_ms,
				.buffered_s = (float)ring_count(&video_ring) * 1000.0f / (float)fps_x1000,
				.all_loaded = ytsrc_is_eof(&src),
				.live = st->is_live,
				.paused = paused,
				.buffering = buffering,
				.stereo = view.layout != VL_2D,
				.audio_ok = audio_ok,
				.mode_name = st->is_360 ? "360\xC2\xB0 video (shown flat)" : play_mode_name(cur_mode),
				.next_mode_name = play_mode_name(next_mode(cur_mode)),
				.errors = decode_errors,
				.linear_free_kb = mem_linear_free() / 1024,
				.heap_free_kb = mem_heap_free() / 1024,
				.mvd_workbuf_kb = mvd_workbuf / 1024,
				.can_switch = can_switch,
				.set_open = set_open,
				.smooth_on = app.motion != MOTION_NORMAL,
				.stats_on = app.nerd_stats,
				.show_depth = view.layout != VL_2D,
				.depth_step = g_depth_step,
				.auto_px = (s8)(auto_conv * screen_scale(view.layout, crop_w)),
				.cc_label = cc_label,
				.cc_on = subs->want >= 0,
				.cc_none = st->caption_count == 0 && !n_audio,
				.cc_menu = cc_menu,
				.cc_names = cc_names,
				.cc_count = st->caption_count,
				.cc_hl = cc_hl,
				.cc_sel = subs->want + 1,
				.cc_top = cc_top,
				.audio_names = audio_names,
				.audio_count = n_audio,
				.audio_sel = src.audio_cur,
				.channel = g_channel[0] ? g_channel : st->author,
				.views = g_views,
				.comments_ok = is_youtube,
				.likes = g_likes,
				.dislikes = g_dislikes,
				.cm_open = g_cm_open,
				.cm = g_cm,
				.cm_count = g_cm_n,
				.cm_loading = g_cm_busy,
				.cm_error = g_cm_err[0] ? g_cm_err : NULL,
			};
			if (subs->want >= 0)
				snprintf(cc_label, sizeof(cc_label), "CC %s%s", st->captions[subs->want].lang,
				         subs->have != subs->want ? (subs->failed == subs->want ? " !" : "...") : "");
			else snprintf(cc_label, sizeof(cc_label), st->caption_count || !n_audio ? "CC Off" : "Audio");
		}
		view.paused = paused;
		view.buffering = buffering;
		view.note = seek_note[0] ? seek_note : NULL;
		// The subtitle track waits until the video is safely buffered (3 s queued, or 4 s into
		// playback). Fetched during the start, it competes with the media download, and on
		// hardware videos sometimes ran dry a few seconds in while a track was still loading.
		const bool settled = play_t0 && (ring_count(&video_ring) * frame_ms >= 3000 || osGetTime() - play_t0 >= 4000);
		sub_update(subs, st, settled);
		// the like and dislike counts wait for the same reason
		if (settled && is_youtube) votes_fetch();
		if (subs->want >= 0 && subs->have == subs->want && !seeking)
		{
			const char* u = st->captions[subs->want].url;
			view.subtitle = !strncmp(u, "emb:", 4)
			                    ? ytsrc_sub_text(&src, (int)strtol(u + 4, NULL, 10), clock, sub_text, sizeof(sub_text))
			                    : subs_at(&subs->subs, clock, sub_text, sizeof(sub_text));
		}
		else view.subtitle = NULL;
		{
			// log the first few subtitle changes of a session, to check they show up on time
			const char* text = view.subtitle ? view.subtitle : "";
			if (sub_logs < 6 && strncmp(text, last_sub, sizeof(last_sub) - 1) != 0)
			{
				sub_logs++;
				snprintf(last_sub, sizeof(last_sub), "%s", text);
				tlog("direct: subtitle at %lu ms: \"%s\"", (unsigned long)clock, last_sub);
			}
		}
		if (q_count > 0 && !buffering && !paused)
		{
			// We're just past a top-screen refresh (the loop waits for one below), so a frame drawn
			// now appears at the next refresh, a known time. Each frame goes on the refresh closest
			// to its time: a 30 fps video gets an even 2 refreshes per frame, 24 fps gets 2,3,2,3.
			const float to_screen = REFRESH_MS - since_refresh(vb_tick);
			// Take the earliest picture in the pool. Pictures that would reach the screen more
			// than 3 frames late are skipped as long as a later one is queued.
			int next;
			for (;;)
			{
				next = pool_earliest(q_used, q_pts);
				s32 behind = (s32)(clock - q_pts[next]) + (s32)to_screen;
				if (!at_refresh || q_count <= 1 || behind <= 3 * (s32)frame_ms) break;
				late++;
				if (late_logged < 8)
				{
					late_logged++;
					tlog("direct: late frame: pts %lu, clock %lu (%ld ms behind), %d decoded frames queued",
					     (unsigned long)q_pts[next], (unsigned long)clock, (long)behind, q_count);
				}
				if (next == blend_slot) blend_slot = -1;
				q_used[next] = false;
				q_count--;
			}
			s32 lateness = (s32)(clock - q_pts[next]) + (s32)to_screen;
			// how many refreshes the current picture will have been up if the next is drawn now
			u32 held_now = shown && vb_at_present ? C3D_FrameCounter(0) - vb_at_present : 99;
			bool due;
			if (smooth_motion && at_refresh && view.tex != NULL && sm_have_cur)
			{
				// ---- Smooth motion. The coming refresh shows the moment at its middle. Pictures
				// whose time has passed become the one on screen, and between that one and the next
				// we draw an in-between picture along the motion vectors.
				due = false;
				u32 vb_now = C3D_FrameCounter(0);
				// The moment this refresh shows comes from a display clock that advances one
				// refresh period per refresh and is pulled gently towards the audio clock. The
				// audio position wobbles by a few ms, and fed straight in, that wobble moves the
				// in-between pictures back and forth. (SVP also times its frames by the display.)
				double target = (double)clock + to_screen + REFRESH_MS / 2;
				if (!sm_disp_ok || vb_now - sm_last_vb > 8 || fabs(target - sm_disp) > 40.0)
				{
					sm_disp = target;   // at the start, or after a jump (seek, stall, pause)
					sm_disp_ok = true;
				}
				else
				{
					sm_disp += (double)(vb_now - sm_last_vb) * REFRESH_MS;
					sm_disp += (target - sm_disp) * 0.05;
				}
				if (sm_last_vb && vb_now - sm_last_vb > 1)
					sm_missed += vb_now - sm_last_vb - 1;   // refreshes the loop missed (e.g. a long decode)
				sm_last_vb = vb_now;
				float mid = (float)sm_disp;
				bool switched = false, draw = false;
				float frac = 0.0f;
				while (next >= 0)
				{
					float span = (float)(s32)(q_pts[next] - sm_cur_pts);
					frac = span > 0.5f ? (mid - (float)sm_cur_pts) / span : 1.0f;
					if (frac < 1.0f) break;
					// from now on, next is the picture on screen
					if (blend_slot != next) show_frame(b, next, &b->tex[back], w, h);
					blend_slot = -1;
					view.tex = &b->tex[back];
					back ^= 1;
					last_slot = next;
					// Bring along its luma map and its motion to the following picture; both are
					// usually worked out ahead, between refreshes.
					sm_cur_ok = sm_luma_ok[next];
					if (sm_cur_ok) memcpy(sm_cur, sm_luma[next], (size_t)sm_lw * sm_lh);
					sm_field_ok = sm_fld_ok[next];
					if (sm_field_ok)
					{
						sm_field = sm_fld[next];
						sm_field_pts = sm_fld_to[next];
						me_vertex_vectors(&sm_field, sm_vx, sm_vy);
					}
					sm_cur_pts = q_pts[next];
					q_used[next] = false;
					q_count--;
					shown++;
					switched = true;
					auto_conv = ease_conv(auto_conv, auto_target);
					next = pool_earliest(q_used, q_pts);
				}
				draw = switched;
				if (next >= 0 && frac > 0.02f)
				{
					// The motion between the picture on screen and the next is normally worked out
					// ahead (smooth_work below). Do it here only if there's still time left to draw.
					bool have_field = sm_field_ok && sm_field_pts == q_pts[next];
					if (!have_field && sm_cur_ok && sm_luma_ok[next] &&
					    since_refresh(vb_tick) + avg_ms(me_ticks, me_n, WORK_GUESS_MS) < REFRESH_MS - 5.0f)
					{
						u64 me_start = svcGetSystemTick();
						me_estimate(sm_cur, sm_luma[next], sm_lw, sm_lh, sm_prev_ok ? &sm_prev : NULL, &sm_field);
						me_vertex_vectors(&sm_field, sm_vx, sm_vy);
						u64 me_time = svcGetSystemTick() - me_start;
						me_ticks += me_time;
						if (me_time > me_max) me_max = me_time;
						me_n++;
						sm_prev = sm_field;
						sm_prev_ok = true;
						sm_field_pts = q_pts[next];
						sm_field_ok = have_field = true;
						if (!sm_field.usable) cuts++;
					}
					if (!have_field)
						late_fields++;   // the motion wasn't ready, so this refresh shows the picture as is
					else if (sm_field.usable)
					{
						if (blend_slot != next)
						{
							show_frame(b, next, &b->tex[back], w, h);   // the spare texture
							blend_slot = next;
						}
						view.tex2 = &b->tex[back];
						// The in-between moments stay evenly spaced. Pulling them towards the
						// nearest decoded picture when the match is weak, as SVP's "adaptive" mode
						// does, knocks the motion off its timeline (judder) and measured no better.
						view.mix = frac;
						view.mvx = sm_vx;
						view.mvy = sm_vy;
						view.mvw = sm_field.bw + 1;
						view.mvh = sm_field.bh + 1;
						view.mcell = ME_BLOCK * ME_SCALE;
						interps++;
						draw = true;
					}
					else if (frac >= 0.5f)
						due = true;   // a scene cut: show the next picture as is from the halfway point on
				}
				if (!draw && !due) sm_idle++;
				if (draw && !due)
				{
					view.conv = auto_conv + g_depth_step * DEPTH_STEP_PX / screen_scale(view.layout, crop_w);
					u64 draw_start = svcGetSystemTick();
					pui_video(&view,
					          ui_due ? &status : NULL);   // the remote goes along, since every refresh draws anyway
					u64 draw_time = svcGetSystemTick() - draw_start;
					draw_ticks += draw_time;
					if (draw_time > draw_max) draw_max = draw_time;
					if (ui_due) last_ui = osGetTime();
					view.tex2 = NULL;
					view.mvx = view.mvy = NULL;
					drew = true;
				}
			}
			else if (held_now < hold_lo && lateness < (s32)REFRESH_MS)
				due = false;   // too early for the cadence, and not a whole refresh behind
			else if (held_now >= hold_hi && lateness >= -(s32)REFRESH_MS)
				due = true;                                  // its turn by the cadence, and not a whole refresh early
			else due = lateness >= -(s32)(REFRESH_MS / 2);   // otherwise the refresh nearest its time
			if (at_refresh && due && next >= 0)
			{
				if (smooth_motion)
				{
					// this becomes the picture on screen, with its luma map and motion if they're ready
					sm_cur_ok = sm_luma_ok[next];
					if (sm_cur_ok) memcpy(sm_cur, sm_luma[next], (size_t)sm_lw * sm_lh);
					sm_field_ok = sm_fld_ok[next];
					if (sm_field_ok)
					{
						sm_field = sm_fld[next];
						sm_field_pts = sm_fld_to[next];
						me_vertex_vectors(&sm_field, sm_vx, sm_vy);
					}
					sm_cur_pts = q_pts[next];
					sm_have_cur = true;
				}
				if (blend_slot != next)   // otherwise it's already in the spare texture
					show_frame(b, next, &b->tex[back], w, h);
				blend_slot = -1;
				last_slot = next;
				q_used[next] = false;
				q_count--;
				view.tex = &b->tex[back];
				back ^= 1;
				auto_conv = ease_conv(auto_conv, auto_target);   // ease convergence changes in
				view.conv = auto_conv + g_depth_step * DEPTH_STEP_PX / screen_scale(view.layout, crop_w);
				u64 draw_start = svcGetSystemTick();
				pui_video(&view, NULL);   // the remote gets its own frame below
				u64 draw_time = svcGetSystemTick() - draw_start;
				draw_ticks += draw_time;
				if (draw_time > draw_max) draw_max = draw_time;
				u32 vb = C3D_FrameCounter(0);
				if (shown && vb_at_present)
				{
					u32 n = vb - vb_at_present;   // how many refreshes the previous frame stayed up
					if (n > cadence + 1) uneven++;
					held[n == 0 ? 0 : n > 5 ? 4 : n - 1]++;
				}
				vb_at_present = vb;
				shown++;
				drew = true;
			}
		}
		// ---- "Nerd stats" (a Settings option), updated once a second in the top-right corner.
		// They show the stream, new pictures on screen per second (in-between ones included) and
		// how many of those were video frames, late and skipped frames, the buffer, decode time,
		// sound and memory.
		if (drew) fps_pics++;
		if (!buffering && !paused)
		{
			u64 now = osGetTime();
			if (!fps_t0)
			{
				fps_t0 = now;
				fps_pics = 0;
				fps_shown0 = shown;
			}
			else if (now - fps_t0 >= 1000)
			{
				double secs = (double)(now - fps_t0) / 1000.0;
				char shrink[40] = "";
				if (vscale > 1)
					snprintf(shrink, sizeof(shrink), " (shown %lux%lu)", (unsigned long)crop_w, (unsigned long)crop_h);
				char sound[40] = "no sound";
				if (audio_active)
					snprintf(sound, sizeof(sound), "sound %.1f kHz %s", src.audio_rate / 1000.0,
					         src.audio_ch == 2 ? "stereo" : "mono");
				snprintf(stats_text, sizeof(stats_text),
				         "%lux%lu%s H.264  \xC2\xB7  %.3f fps  \xC2\xB7  %s\n"
				         "%.0f fps on screen (%.0f video)  \xC2\xB7  late %lu  \xC2\xB7  skipped %lu\n"
				         "buffer %.1f s  \xC2\xB7  decode %.1f ms  \xC2\xB7  motion %s\n"
				         "%s  \xC2\xB7  layout %s\n"
				         "free %.1f + %.0f MB  \xC2\xB7  decoder %.1f MB",
				         (unsigned long)src.crop_w, (unsigned long)src.crop_h, shrink, fps_x1000 / 1000.0,
				         ytsrc_is_file(st->muxed_url) ? "file"
				         : dash                       ? "YouTube DASH"
				                                      : "YouTube 360p",
				         fps_pics / secs, (shown - fps_shown0) / secs, (unsigned long)late, (unsigned long)skipped_late,
				         (double)ring_count(&video_ring) * frame_ms / 1000.0,
				         dec_n ? (double)dec_ticks / dec_n / CPU_TICKS_PER_MSEC : 0.0,
				         smooth_motion ? "smooth" : "normal", sound, play_mode_name(cur_mode),
				         mem_linear_free() / 1048576.0, mem_heap_free() / 1048576.0, mvd_workbuf / 1048576.0);
				view.stats = app.nerd_stats ? stats_text : NULL;
				fps_t0 = now;
				fps_pics = 0;
				fps_shown0 = shown;
			}
		}
		else fps_t0 = 0;
		// Right after a video frame the remote waits for the next pass, while the GPU is free and
		// the next frame is still a while off.
		if (!drew && (ui_due || buffering || paused))
		{
			if (ui_due && view.tex && !buffering && !paused)
				pui_status(&status);   // just the remote, without waiting on the top screen
			else if (ui_due || !view.tex) { pui_video(&view, ui_due ? &status : NULL); }
			if (ui_due) last_ui = osGetTime();
		}

		// ---- the 5-second playback summary (see win_tick)
		if (!buffering && !paused)
		{
			u64 win_now = svcGetSystemTick();
			if (!win_tick)
			{
				win_after_start = true;   // win_tick is 0 after every start, seek, pause and buffering
				win_tick = win_now;
				win_clock = clock;
				win_shown = shown;
				win_interps = interps;
				win_repeats = repeats;
				win_late = late;
			}
			else if (win_now - win_tick >= STATS_WINDOW_MS * (u64)CPU_TICKS_PER_MSEC)
			{
				double wall = (double)(win_now - win_tick) / CPU_TICKS_PER_MSEC;
				tlog("direct: 5 s: shown %lu (+%lu in-between), late %lu, repeats dropped %lu, clock rate %.3f, queued "
				     "%lu video / %lu audio, wavebufs %d",
				     (unsigned long)(shown - win_shown), (unsigned long)(interps - win_interps),
				     (unsigned long)(late - win_late), (unsigned long)(repeats - win_repeats),
				     (double)(s32)(clock - win_clock) / wall, (unsigned long)ring_count(&video_ring),
				     (unsigned long)ring_count(&audio_ring), wb_submitted);
				// A window that began as playback started doesn't count. The first pictures after a
				// start or seek can be late while the decoder fills the pool, and a handful of those
				// shouldn't switch Smooth off for the whole video.
				if (smooth_motion && late - win_late > 5 && !win_after_start)
				{
					// The console can't keep up with in-between pictures for this video, so fall
					// back to plain frames (with the cadence lock) from here on.
					smooth_motion = false;
					sm_cur_ok = sm_field_ok = false;
					tlog("direct: smooth motion off: %lu late frames in 5 s", (unsigned long)(late - win_late));
				}
				win_tick = win_now;
				win_after_start = false;
				win_clock = clock;
				win_shown = shown;
				win_interps = interps;
				win_repeats = repeats;
				win_late = late;
			}
		}
		else win_tick = 0;   // pausing or buffering starts a new window

		// ---- end of stream
		if ((ytsrc_is_eof(&src) || ytsrc_has_failed(&src)) && ring_count(&video_ring) == 0 && q_count == 0 &&
		    (!audio_active || !use_audio_clock || (ring_count(&audio_ring) == 0 && wb_submitted == 0)))
		{
			if (ytsrc_has_failed(&src) && (!duration_ms || clock + 5000 < duration_ms))
			{
				*pos_ms = clock;
				tlog("direct: stream failed at %lu ms: %s%s", (unsigned long)clock, src.errmsg,
				     can_fall_back ? " (continuing with the muxed stream)" : "");
				if (can_fall_back) action = D_MUXED;   // e.g. YouTube cut the DASH streams off part-way
				else if (src.http_status == 403 || src.http_status == 404 || src.http_status == 410)
					action = D_REFRESH;   // the link expired (they last a few hours), so ask YouTube again
				else
				{
					char msg[400], at_text[16];
					fmt_time(at_text, sizeof(at_text), clock);
					snprintf(msg, sizeof(msg), "%s (at %s)", src.errmsg, at_text);
					playback_home_block(false);   // HOME works normally in the dialog
					// Resume asks YouTube for fresh links, since the old ones may have expired.
					action = pui_message("Playback stopped", msg, "Resume", "Back") ? D_REFRESH : D_STOP;
				}
			}
			else action = D_ENDED;
			break;
		}

		// ---- Smooth motion work between refreshes. The luma maps of the pooled pictures and the
		// motion between consecutive ones are worked out ahead, one piece at a time, and only
		// while it fits before the next refresh (whose picture has to be drawn right after it).
		// When the pool runs low, decoding gets priority instead.
		bool sm_pending = false;
		if (smooth_motion && !paused && !buffering && !(q_count < 3 && ring_count(&video_ring) > 0))
		{
			int order[FRAME_QUEUE], n_order = 0;   // pooled pictures in display order
			for (int i = 0; i < FRAME_QUEUE; i++)
				if (q_used[i])
				{
					int o = n_order++;
					while (o > 0 && (s32)(q_pts[order[o - 1]] - q_pts[i]) > 0)
					{
						order[o] = order[o - 1];
						o--;
					}
					order[o] = i;
				}
			const float since_vb = since_refresh(vb_tick), budget = REFRESH_MS - REFRESH_SLACK_MS - since_vb;
			int luma_slot = -1;
			for (int o = 0; o < n_order && luma_slot < 0; o++)
				if (!sm_luma_ok[order[o]]) luma_slot = order[o];
			if (luma_slot >= 0)
			{
				sm_pending = true;
				if (avg_ms(luma_ticks, luma_n, WORK_GUESS_MS) <
				    budget)   // the worker normally does this; here only what fits
				{
					u64 luma_start = svcGetSystemTick();
					dma_read_barrier(b->tiled[luma_slot],
					                 b->frame_bytes);   // written by the GPU (same size as an MVD output)
					me_luma_tiled(
					    (const u16*)b->tiled[luma_slot], (int)w, (int)(motion_layout == VL_SBS ? crop_w / 2 : crop_w),
					    (int)(motion_layout == VL_TB ? crop_h / 2 : crop_h), sm_luma[luma_slot], &sm_lw, &sm_lh);
					sm_luma_ok[luma_slot] = true;
					luma_ticks += svcGetSystemTick() - luma_start;
					luma_n++;
				}
			}
			else
			{
				// Find the first motion still missing: from the picture on screen to the first
				// pooled one, then from each pooled picture to the one after it.
				const u8* from_luma = NULL;
				const u8* to_luma = NULL;
				MotionField* field_out = NULL;
				const MotionField* seed = NULL;
				u32 to_pts = 0;
				int field_slot = -1;   // slot the field belongs to, or -2 for the picture on screen
				if (n_order > 0 && sm_have_cur && sm_cur_ok && !(sm_field_ok && sm_field_pts == q_pts[order[0]]))
				{
					from_luma = sm_cur;
					to_luma = sm_luma[order[0]];
					field_out = &sm_field;
					to_pts = q_pts[order[0]];
					field_slot = -2;
					seed = sm_prev_ok ? &sm_prev : NULL;
				}
				else
					for (int o = 0; o + 1 < n_order; o++)
					{
						int slot = order[o], next_slot = order[o + 1];
						if (sm_fld_ok[slot] && sm_fld_to[slot] == q_pts[next_slot]) continue;
						from_luma = sm_luma[slot];
						to_luma = sm_luma[next_slot];
						field_out = &sm_fld[slot];
						to_pts = q_pts[next_slot];
						field_slot = slot;
						seed = o > 0 && sm_fld_ok[order[o - 1]] ? &sm_fld[order[o - 1]]
						       : sm_field_ok                    ? &sm_field
						                                        : NULL;
						break;
					}
				if (field_out)
				{
					sm_pending = true;
					if (avg_ms(me_ticks, me_n, WORK_GUESS_MS) < budget)
					{
						u64 me_start = svcGetSystemTick();
						me_estimate(from_luma, to_luma, sm_lw, sm_lh, seed, field_out);
						u64 me_time = svcGetSystemTick() - me_start;
						me_ticks += me_time;
						if (me_time > me_max) me_max = me_time;
						me_n++;
						if (!field_out->usable) cuts++;
						if (field_slot == -2)
						{
							me_vertex_vectors(&sm_field, sm_vx, sm_vy);
							sm_field_pts = to_pts;
							sm_field_ok = true;
							sm_prev = sm_field;
							sm_prev_ok = true;
						}
						else
						{
							sm_fld_to[field_slot] = to_pts;
							sm_fld_ok[field_slot] = true;
						}
					}
				}
			}
		}

		// Sleep until the next top-screen refresh (where the present step runs), unless there's
		// something to do right now: a finished picture to collect, or an access unit for an idle
		// worker (only one per refresh while buffering with pictures already queued). While the
		// worker decodes, wake for whichever comes first, its picture or the refresh.
		at_refresh = false;
		bool now_work = (wk.busy && wk.done) || (!wk.busy && q_count <= pool - MVD_OUTS &&
		                                         ring_count(&video_ring) > 0 && !paused && !(buffering && q_count > 0));
		if (!now_work && smooth_motion && sm_pending && !paused && !buffering)
		{
			float left = REFRESH_MS - REFRESH_SLACK_MS - since_refresh(vb_tick);
			now_work = fminf(avg_ms(luma_ticks, luma_n, WORK_GUESS_MS), avg_ms(me_ticks, me_n, WORK_GUESS_MS)) < left;
		}
		if (!now_work)
		{
			// A refresh that happened while this pass ran counts right away (vb_seen is the last
			// one the loop woke for), so its picture is a few ms late rather than a whole refresh.
			if (!wk.busy && (s32)(C3D_FrameCounter(0) - vb_seen) <= 0)
			{
				gspWaitForVBlank0();
				// citro3d's event callback may bump its counter a moment after we wake up. Either
				// way, this refresh is the one after vb_seen.
				u32 vb_now = C3D_FrameCounter(0);
				vb_seen = (s32)(vb_now - vb_seen) > 0 ? vb_now : vb_seen + 1;
				vb_tick = svcGetSystemTick();
				at_refresh = true;
			}
			else
			{
				while ((s32)(C3D_FrameCounter(0) - vb_seen) <= 0 && !wk.done) svcSleepThread(500000ULL);
				u32 vb_now = C3D_FrameCounter(0);
				if ((s32)(vb_now - vb_seen) > 0)
				{
					vb_seen = vb_now;
					vb_tick = svcGetSystemTick();
					at_refresh = true;
				}
			}
		}
	}

	// Leaving the session (after B we go back to the menu). Each step is timed for the log.
	u64 leave_t[7];
	leave_t[0] = osGetTime();
	power_bottom_backlight(true);
	ytsrc_stop(&src);
	leave_t[1] = osGetTime();
	if (audio_active)
	{
		ndspChnWaveBufClear(AUDIO_CHANNEL);
		ndspChnReset(AUDIO_CHANNEL);
		svcSleepThread(20 * 1000000ULL);   // let the DSP release the buffers
	}
	if (wb_mem) linearFree(wb_mem);
	leave_t[2] = osGetTime();
	mvd_worker_stop();
	leave_t[3] = osGetTime();
	leave_t[4] = osGetTime();
	if (mvd_ready) mvdstdExit();
	leave_t[5] = osGetTime();
	playback_home_block(false);
	leave_t[6] = osGetTime();
	tlog("direct: leaving took %llu ms: source %llu, audio %llu, decoder thread %llu, MVD %llu, HOME %llu",
	     (unsigned long long)(leave_t[6] - leave_t[0]), (unsigned long long)(leave_t[1] - leave_t[0]),
	     (unsigned long long)(leave_t[2] - leave_t[1]), (unsigned long long)(leave_t[3] - leave_t[2]),
	     (unsigned long long)(leave_t[5] - leave_t[4]), (unsigned long long)(leave_t[6] - leave_t[5]));
	if (pui_gpu_recoveries())
		tlog("direct: GPU queue recovered %lu times so far (a lost GPU interrupt, see ui.c)",
		     (unsigned long)pui_gpu_recoveries());
	if (skipped_late)
		tlog("direct: %lu unreferenced pictures left undecoded (MVD behind)", (unsigned long)skipped_late);
	tlog("direct: session end at %lu ms: action %d, shown %lu, late %lu, decode errors %lu, underruns %lu, seeks %lu",
	     (unsigned long)clock, (int)action, (unsigned long)shown, (unsigned long)late, (unsigned long)decode_errors,
	     (unsigned long)underruns, (unsigned long)seeks);
	tlog("direct: loop stalls > 50 ms: %lu, longest %.0f ms; frames skipped before the audio start: %lu",
	     (unsigned long)stalls, (double)stall_max / CPU_TICKS_PER_MSEC, (unsigned long)skipped);
	tlog("direct: frames held for 1/2/3/4/5+ refreshes: %lu/%lu/%lu/%lu/%lu", (unsigned long)held[0],
	     (unsigned long)held[1], (unsigned long)held[2], (unsigned long)held[3], (unsigned long)held[4]);
	tlog("direct: MVD pictures: %lu written while processing, %lu fetched by render (%lu render calls) for %lu access "
	     "units",
	     (unsigned long)wk.written_in_process, (unsigned long)wk.written_by_render, (unsigned long)wk.renders,
	     (unsigned long)au_seen);
	if (smooth_motion)
		tlog("direct: smooth motion: %lu in-between pictures for %lu decoded (%lu cuts, %lu repeated frames dropped, "
		     "%lu refreshes without a new "
		     "picture + %lu missed, %lu without motion ready); luma avg %.1f ms, motion search avg %.1f max %.1f ms",
		     (unsigned long)interps, (unsigned long)shown, (unsigned long)cuts, (unsigned long)repeats,
		     (unsigned long)sm_idle, (unsigned long)sm_missed, (unsigned long)late_fields,
		     luma_n ? (double)luma_ticks / luma_n / CPU_TICKS_PER_MSEC : 0.0,
		     me_n ? (double)me_ticks / me_n / CPU_TICKS_PER_MSEC : 0.0, (double)me_max / CPU_TICKS_PER_MSEC);
	else if (app.motion != MOTION_NORMAL)
		tlog("direct: smooth motion off for this video (%.1f refreshes per frame, mesh %s)", (double)per_frame,
		     vmesh_ready() ? "ready" : "not ready");
	tlog("direct: timing: decode avg %.1f max %.1f ms, draw avg %.1f max %.1f ms, uneven frames %lu (cadence %lu)",
	     dec_n ? (double)dec_ticks / dec_n / CPU_TICKS_PER_MSEC : 0.0, (double)dec_max / CPU_TICKS_PER_MSEC,
	     shown ? (double)draw_ticks / shown / CPU_TICKS_PER_MSEC : 0.0, (double)draw_max / CPU_TICKS_PER_MSEC,
	     (unsigned long)uneven, (unsigned long)cadence);
	return action;
}

// ---------------------------------------------------------------- public entry

// Plays a whole video: allocates the buffers that last for all of it and runs sessions until
// it stops. Seeks happen inside a session; the muxed fallback and fresh links start a new one.
// st is freed here. YouTube videos and files both come through here; a file's st has only
// muxed_url (its path) and the title.
static void play_streams(const char* video_id, YtStreams st, const char* title, PlayMode* mode, bool audio);

void direct_play(const char* video_id, const char* title, PlayMode* mode, bool audio)
{
	snprintf(g_video_id, sizeof(g_video_id), "%s", video_id);
	comments_clear();
	// ---- find the streams (a few network round trips) while the loading screen shows
	YtStreams st;
	FindResult fr = find_streams(video_id, &st, "Loading video");
	if (fr == FIND_CANCELLED) return;
	if (fr == FIND_FAILED)
	{
		tlog("direct: %s: no streams: %s", video_id, st.error);
		pui_message("Can't play this video", st.error, NULL, "Back");
		yt_free_streams(&st);
		return;
	}
	play_streams(video_id, st, title, mode, audio);
	comments_clear();
	g_channel[0] = g_views[0] = 0;   // the caller sets them again for the next video
}

void direct_play_file(const char* path, const char* title, PlayMode* mode, bool audio)
{
	snprintf(g_video_id, sizeof(g_video_id), "file");
	comments_clear();
	g_channel[0] = g_views[0] = 0;
	YtStreams st;
	memset(&st, 0, sizeof(st));
	str_copy(st.title, sizeof(st.title), title);
	st.muxed_url = strdup(path);
	if (!st.muxed_url) return;
	tlog("direct: file %s", path);
	play_streams("file", st, title, mode, audio);
}

static void play_streams(const char* video_id, YtStreams st, const char* title, PlayMode* mode, bool audio)
{
	direct_mvd_lock();   // waits for any preview being decoded to finish (filethumb.c)

	// ---- buffers that live for the whole video, reused across seeks
	Buffers b;
	memset(&b, 0, sizeof(b));
	b.nal = (u8*)linearMemAlign(NAL_BUF_SIZE, 0x40);
	b.vring = (u8*)malloc(VRING_BYTES);
	b.aring = (u8*)malloc(ARING_BYTES);
	if (!b.nal || !b.vring || !b.aring)
	{
		pui_message("Not enough memory", "There isn't enough free memory to play a video.", NULL, "Back");
	}
	else
	{
		aptSetSleepAllowed(false);   // closing the lid would drop Wi-Fi mid-stream
		tlog("direct: %s: %s, %lu s", video_id,
		     st.video_url && st.audio_url  ? "DASH"
		     : ytsrc_is_file(st.muxed_url) ? "a file"
		                                   : "muxed 360p",
		     (unsigned long)(st.duration_ms / 1000));
		u32 pos = 0;
		bool use_muxed = false;
		int refreshes = 0;
		int audio_stream = -1;   // a file's audio track
		const bool file = ytsrc_is_file(st.muxed_url);
		SubState subs = { .want = -1, .have = -1, .failed = -1, .start_choice = -2 };
		ResumeInfo saved;
		if (file)
		{
			add_sidecar_subs(&st, st.muxed_url);   // embedded tracks are added once the file is open
			if (resume_get(st.muxed_url, &saved))
			{
				audio_stream = saved.audio;
				subs.start_choice = saved.sub;
				if (saved.pos_ms)
				{
					char at[16], msg[96];
					fmt_time(at, sizeof(at), saved.pos_ms);
					snprintf(msg, sizeof(msg), "You stopped at %s last time.", at);
					if (pui_message("Continue watching?", msg, "Resume", "From the start")) pos = saved.pos_ms;
					tlog("direct: resume point %lu ms: %s", (unsigned long)saved.pos_ms,
					     pos ? "resuming" : "from the start");
				}
			}
		}
		else
		{
			subs.want = app.subtitles ? sub_default(&st) : -1;
			tlog("direct: %d subtitle tracks, starting with %s", st.caption_count,
			     subs.want < 0 ? "none" : st.captions[subs.want].lang);
		}
		DAction action = D_STOP;
		for (;;)
		{
			action = run_session(&st, title, mode, &pos, audio, &b, use_muxed, &subs, &audio_stream);
			if (action == D_STOP || action == D_ENDED || aptShouldClose()) break;
			if (action == D_MUXED)
			{
				tlog("direct: switching to the muxed stream at %lu ms", (unsigned long)pos);
				use_muxed = true;
			}
			else if (action == D_REFRESH && !st.video_url && !st.audio_url && ytsrc_is_file(st.muxed_url))
				break;   // a file has no links to fetch again
			else if (action == D_REFRESH)
			{
				// Get fresh links from YouTube (they expire, or were refused), but only a few times.
				if (++refreshes > 3) break;
				YtStreams fresh;
				FindResult found = find_streams(video_id, &fresh, "Reconnecting");
				if (found != FIND_OK)
				{
					if (found == FIND_FAILED)
					{
						pui_message("Can't play this video", fresh.error, NULL, "Back");
						yt_free_streams(&fresh);
					}
					break;
				}
				yt_free_streams(&st);
				st = fresh;
				use_muxed = false;
				if (subs.want >= st.caption_count) subs.want = -1;   // the track list may have changed
				if (subs.have >= st.caption_count) subs.have = -1;
				tlog("direct: fresh links (%s)", st.video_url && st.audio_url ? "DASH" : "muxed 360p");
			}
		}
		if (file)
		{
			// Remember where it was left and which tracks were picked. A video watched to the end
			// starts over next time.
			ResumeInfo now = {
				.pos_ms = action == D_ENDED ? st.duration_ms : pos,
				.dur_ms = st.duration_ms,
				.audio = audio_stream,
				.sub = subs.chosen ? subs.want : subs.start_choice,
			};
			resume_set(st.muxed_url, &now);
			tlog("direct: left at %lu of %lu ms (%s)", (unsigned long)now.pos_ms, (unsigned long)now.dur_ms,
			     resume_worth(now.pos_ms, now.dur_ms) ? "resume point saved" : "no resume point");
		}
		sub_stop(&subs);
		aptSetSleepAllowed(true);
	}
	free_frame_buffers(&b);
	if (b.nal) linearFree(b.nal);
	free(b.vring);
	free(b.aring);
	yt_free_streams(&st);
	direct_mvd_unlock();
	tlog("direct: video closed");
}
