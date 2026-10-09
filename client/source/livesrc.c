#include "livesrc.h"
#include "tlog.h"
#include "yt.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIVE_BEHIND    3   // start this many chunks behind the live edge (~15 s)
#define PLAYLIST_BYTES (128 * 1024)
#define MAX_SEGS       64      // most chunks we keep from one playlist (ANDROID lists about 7)
#define STALL_MS       40000   // no new chunk for this long means the stream is gone
#define STACK_BYTES    (32 * 1024)

typedef struct {
	long long seq;   // the chunk's media sequence number
	char* url;
} Seg;

struct LiveSrc {
	char* url;         // the media playlist
	u8* buf;           // byte FIFO the chunks are appended to
	u32 cap, rd, wr;   // wr - rd bytes are buffered; both only grow and are used mod cap
	LightLock lock;
	volatile bool quit;
	bool ended;   // saw #EXT-X-ENDLIST and downloaded every chunk
	char error[160];
	int http_status;
	Thread thread;
};

// ---------------------------------------------------------------- HTTP

typedef struct {
	LiveSrc* s;
	char* text;   // a playlist is collected here
	size_t len;
	bool to_fifo;   // a chunk goes straight into the FIFO instead
	long long bytes;
} Fetch;

static u32 fifo_free(LiveSrc* s) { return s->cap - (s->wr - s->rd); }

// Copy n bytes into or out of the FIFO at position at, wrapping around the end of the buffer.
static void fifo_put(LiveSrc* s, u32 at, const u8* src, u32 n)
{
	u32 offset = at % s->cap, first = n < s->cap - offset ? n : s->cap - offset;
	memcpy(s->buf + offset, src, first);
	memcpy(s->buf, src + first, n - first);
}
static void fifo_get(LiveSrc* s, u32 at, u8* dst, u32 n)
{
	u32 offset = at % s->cap, first = n < s->cap - offset ? n : s->cap - offset;
	memcpy(dst, s->buf + offset, first);
	memcpy(dst + first, s->buf, n - first);
}

static size_t on_data(char* data, size_t size, size_t n, void* user)
{
	Fetch* f = (Fetch*)user;
	size_t total = size * n;
	if (!f->to_fifo)
	{
		if (f->len + total >= PLAYLIST_BYTES) return 0;
		memcpy(f->text + f->len, data, total);
		f->len += total;
		f->text[f->len] = 0;
		return total;
	}
	LiveSrc* s = f->s;
	size_t done = 0;
	while (done < total)
	{
		if (s->quit || yt_aborting()) return 0;
		LightLock_Lock(&s->lock);
		u32 room = fifo_free(s);
		u32 chunk = (u32)(total - done) < room ? (u32)(total - done) : room;
		fifo_put(s, s->wr, (const u8*)data + done, chunk);
		s->wr += chunk;
		LightLock_Unlock(&s->lock);
		done += chunk;
		if (chunk == 0) svcSleepThread(10 * 1000000ULL);   // full: we're ahead of the demuxer
	}
	f->bytes += (long long)total;
	return total;
}

static int on_progress(void* user, curl_off_t dl_total, curl_off_t dl_now, curl_off_t ul_total, curl_off_t ul_now)
{
	(void)dl_total;
	(void)dl_now;
	(void)ul_total;
	(void)ul_now;
	Fetch* f = (Fetch*)user;
	return (f->s->quit || yt_aborting()) ? 1 : 0;
}

// Fetches url into the FIFO (for a chunk) or into f->text (for a playlist). Returns the HTTP
// status, or -1 if the transfer itself failed.
static long http_get(CURL* curl, const char* url, Fetch* f)
{
	curl_easy_reset(curl);
	yt_setup_curl(curl);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
	curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 32768L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, f);
	CURLcode rc = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	return rc == CURLE_OK ? status : -1;
}

// ---------------------------------------------------------------- the playlist

// Parses an HLS media playlist: #EXT-X-MEDIA-SEQUENCE gives the first chunk's number, and
// each #EXTINF line is followed by a chunk URL. Returns how many chunks were found (their URLs
// strdup'ed into segs) and sets *end if the playlist has #EXT-X-ENDLIST.
static int parse_playlist(char* text, Seg* segs, int max, bool* end, int* target_s)
{
	long long seq = 0;
	int n = 0;
	*end = false;
	*target_s = 5;
	char* save = NULL;
	for (char* line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save))
	{
		if (!strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22)) seq = strtoll(line + 22, NULL, 10);
		else if (!strncmp(line, "#EXT-X-TARGETDURATION:", 22))
		{
			*target_s = (int)strtol(line + 22, NULL, 10);
			if (*target_s <= 0 || *target_s > 30) *target_s = 5;
		}
		else if (!strncmp(line, "#EXT-X-ENDLIST", 14)) *end = true;
		else if (line[0] && line[0] != '#')
		{
			if (n < max)
			{
				segs[n].seq = seq;
				segs[n].url = strdup(line);
				if (segs[n].url) n++;
			}
			else
			{
				// keep only the newest max chunks by dropping the oldest
				free(segs[0].url);
				memmove(segs, segs + 1, sizeof(Seg) * (max - 1));
				segs[max - 1].seq = seq;
				segs[max - 1].url = strdup(line);
				if (!segs[max - 1].url) n--;
			}
			seq++;
		}
	}
	return n;
}

static void set_error(LiveSrc* s, const char* msg, int status)
{
	LightLock_Lock(&s->lock);
	if (!s->error[0])
	{
		snprintf(s->error, sizeof(s->error), "%s", msg);
		s->http_status = status;
	}
	LightLock_Unlock(&s->lock);
}

static void live_main(void* arg)
{
	LiveSrc* s = (LiveSrc*)arg;
	CURL* curl = curl_easy_init();
	char* text = (char*)malloc(PLAYLIST_BYTES);
	Seg* segs = (Seg*)calloc(MAX_SEGS, sizeof(Seg));
	if (!curl || !text || !segs)
	{
		set_error(s, "Out of memory for the live stream.", 0);
		goto out;
	}
	long long next = -1;          // the next chunk to download
	u64 last_new = osGetTime();   // when a new chunk last appeared
	int failures = 0;
	while (!s->quit && !yt_aborting())
	{
		Fetch playlist_fetch = { .s = s, .text = text };
		text[0] = 0;
		long status = http_get(curl, s->url, &playlist_fetch);
		if (s->quit || yt_aborting()) break;
		if (status != 200)
		{
			if (status == 403 || status == 404 || status == 410)
			{
				char msg[96];
				snprintf(msg, sizeof(msg), "The live stream was refused (HTTP %ld).", status);
				set_error(s, msg, (int)status);
				break;
			}
			if (++failures >= 6)
			{
				set_error(s, "The live stream can't be reached (network).", 0);
				break;
			}
			svcSleepThread(1000 * 1000000ULL);
			continue;
		}
		bool end;
		int target_s;
		int n = parse_playlist(text, segs, MAX_SEGS, &end, &target_s);
		if (n > 0)
		{
			const long long first = segs[0].seq, last = segs[n - 1].seq;
			const long long near_edge = last - (LIVE_BEHIND - 1) < first ? first : last - (LIVE_BEHIND - 1);
			if (next < 0)
			{
				next = near_edge;
				tlog("live: chunks %lld..%lld, starting at %lld (target %d s)", first, last, next, target_s);
			}
			else if (next < first)
			{
				// A long stall left us behind the playlist's window, so catch up near the edge.
				tlog("live: behind the window (%lld < %lld): jumping to the edge", next, first);
				next = near_edge;
			}
			for (int i = 0; i < n && !s->quit; i++)
			{
				if (segs[i].seq < next) continue;
				// Wait until there's room for most of a chunk (playback may be paused or in
				// HOME). A download that sat on a full buffer for long would trip curl's
				// low-speed limit and leave a hole in the stream.
				for (;;)
				{
					LightLock_Lock(&s->lock);
					const u32 room = fifo_free(s);
					LightLock_Unlock(&s->lock);
					if (room >= s->cap / 2 || s->quit || yt_aborting()) break;
					svcSleepThread(20 * 1000000ULL);
				}
				Fetch chunk_fetch = { .s = s, .to_fifo = true };
				int tries = 0;
				long chunk_status;
				do chunk_status = http_get(curl, segs[i].url, &chunk_fetch);
				while (chunk_status != 200 && chunk_fetch.bytes == 0 && ++tries < 3 && !s->quit && !yt_aborting());
				if (chunk_status != 200 && !s->quit)
					tlog("live: chunk %lld: HTTP %ld after %d tries, skipped", segs[i].seq, chunk_status, tries);
				next = segs[i].seq + 1;
				last_new = osGetTime();
				failures = 0;
			}
		}
		for (int i = 0; i < n; i++) free(segs[i].url);
		if (end && next > 0)
		{
			LightLock_Lock(&s->lock);
			s->ended = true;
			LightLock_Unlock(&s->lock);
			tlog("live: the broadcast ended");
			break;
		}
		if (osGetTime() - last_new > STALL_MS)
		{
			set_error(s, "The live stream stopped sending.", 0);
			break;
		}
		// A new chunk shows up every target_s seconds; check again after about half that.
		for (int w = 0; w < target_s * 10 / 2 && !s->quit; w++) svcSleepThread(100 * 1000000ULL);
	}
out:
	free(segs);
	free(text);
	if (curl) curl_easy_cleanup(curl);
}

// ---------------------------------------------------------------- public

LiveSrc* ls_open(const char* url, u32 buf_bytes)
{
	LiveSrc* s = (LiveSrc*)calloc(1, sizeof(LiveSrc));
	if (!s) return NULL;
	s->url = strdup(url);
	s->buf = (u8*)malloc(buf_bytes);
	s->cap = buf_bytes;
	LightLock_Init(&s->lock);
	if (!s->url || !s->buf)
	{
		ls_close(s);
		return NULL;
	}
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// Same placement as hstream.c: the extra core if there is one, otherwise below the caller.
	s->thread = threadCreate(live_main, s, STACK_BYTES, prio - 1, 2, false);
	if (!s->thread) s->thread = threadCreate(live_main, s, STACK_BYTES, prio + 1, -1, false);
	if (!s->thread)
	{
		ls_close(s);
		return NULL;
	}
	return s;
}

int ls_read(LiveSrc* s, u8* buf, int len, volatile bool* stop)
{
	for (;;)
	{
		if (*stop) return -1;
		LightLock_Lock(&s->lock);
		u32 have = s->wr - s->rd;
		if (have > 0)
		{
			u32 n = have < (u32)len ? have : (u32)len;
			fifo_get(s, s->rd, buf, n);
			s->rd += n;
			LightLock_Unlock(&s->lock);
			return (int)n;
		}
		bool ended = s->ended, failed = s->error[0] != 0;
		LightLock_Unlock(&s->lock);
		if (ended) return 0;
		if (failed) return -1;
		svcSleepThread(10 * 1000000ULL);
	}
}

bool ls_error(LiveSrc* s, char* out, size_t len)
{
	LightLock_Lock(&s->lock);
	bool failed = s->error[0] != 0;
	if (failed && out && len) snprintf(out, len, "%s", s->error);
	LightLock_Unlock(&s->lock);
	return failed;
}

int ls_http_status(LiveSrc* s) { return s->http_status; }

void ls_close(LiveSrc* s)
{
	if (!s) return;
	s->quit = true;
	if (s->thread)
	{
		threadJoin(s->thread, U64_MAX);
		threadFree(s->thread);
	}
	free(s->buf);
	free(s->url);
	free(s);
}
