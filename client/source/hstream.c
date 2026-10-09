#include "hstream.h"
#include "yt.h"
#include "tlog.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CHUNK_BYTES   (1024 * 1024)   // bytes per Range request
#define HISTORY_BYTES (64 * 1024)     // already-read bytes kept for short backward seeks
#define MAX_RETRIES   5
#define STACK_BYTES   (48 * 1024)   // download thread stack; curl and mbedTLS handshakes need this much

// A ring buffer indexed by file offset: byte N of the file lives at buf[N % cap]. It holds
// the bytes from base up to (not including) end, and the reader sits at pos somewhere in
// between. The writer is free to overwrite anything older than pos - HISTORY_BYTES.
struct HStream {
	int core;   // download thread's core: 2 (the New 3DS extra core) or -1 (shared)
	char* url;
	u8* buf;
	u32 cap;
	LightLock lock;
	int64_t base, pos, end;
	int64_t size;   // -1 until a Content-Range tells us
	u32 gen;        // bumped on every seek that needs a new download
	bool eof;       // download reached the end of the file (for this gen)
	bool quit;
	char error[160];   // fatal error of the current download; a seek that starts a new
	                   // download clears it
	int err_status;    // HTTP status behind the error (403, 404 or 410), else 0
	Thread thread;
};

// ---------------------------------------------------------------- download thread

typedef struct {
	HStream* s;
	u32 gen;
	int64_t at;   // file offset of the next byte this transfer delivers
	long status;
	bool range_ok;   // server honoured the Range request
	bool got_status;
	bool waiting;             // the buffer filled up and we're letting it drain (see on_data)
	u64 start_ms, first_ms;   // when the request started, and how long until its first byte (0 = none yet)
	u64 wait_ms;              // time spent waiting for buffer room, which isn't the network's fault
} Transfer;

static u32 free_space(HStream* s)
{
	int64_t keep_from = s->pos - HISTORY_BYTES;
	if (keep_from < s->base) keep_from = s->base;
	return s->cap - (u32)(s->end - keep_from);
}

static size_t on_header(char* data, size_t size, size_t n, void* user)
{
	Transfer* t = (Transfer*)user;
	size_t len = size * n;
	// curl's header data isn't NUL-terminated, so parse a bounded, terminated copy. The two
	// headers we care about are short, and cutting a longer line can't make it match wrongly.
	char line[256];
	size_t line_len = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
	memcpy(line, data, line_len);
	line[line_len] = 0;
	if (line_len > 5 && !strncmp(line, "HTTP/", 5))
	{
		const char* space = strchr(line, ' ');
		t->status = space ? strtol(space + 1, NULL, 10) : 0;
		t->got_status = true;
	}
	else if (line_len > 14 && !strncasecmp(line, "content-range:", 14))
	{
		// "Content-Range: bytes 0-1048575/12345678"
		const char* slash = strchr(line, '/');
		if (slash && slash[1] != '*')
		{
			int64_t total = strtoll(slash + 1, NULL, 10);
			LightLock_Lock(&t->s->lock);
			if (t->gen == t->s->gen && total > 0) t->s->size = total;
			LightLock_Unlock(&t->s->lock);
		}
		t->range_ok = true;
	}
	return len;
}

static size_t on_data(char* data, size_t size, size_t n, void* user)
{
	Transfer* t = (Transfer*)user;
	HStream* s = t->s;
	size_t len = size * n, done = 0;
	if (!t->first_ms) t->first_ms = osGetTime() - t->start_ms;
	if (t->status != 206 && !(t->status == 200 && t->at == 0))
		return 0;   // an error page, or the server ignored our Range: abort
	while (done < len)
	{
		LightLock_Lock(&s->lock);
		if (s->quit || t->gen != s->gen)
		{
			LightLock_Unlock(&s->lock);
			return 0;   // a seek or close happened, so this transfer is no longer wanted
		}
		u32 room = free_space(s);
		if (room == 0 || (t->waiting && room < s->cap / 4))
		{
			// The buffer is full. Wait until a quarter of it has drained before downloading
			// again, so Wi-Fi works in bursts and can idle in between (better for the
			// battery), and this thread doesn't wake up for every few KB the reader takes.
			t->waiting = true;
			LightLock_Unlock(&s->lock);
			svcSleepThread(10 * 1000000ULL);
			t->wait_ms += 10;
			continue;
		}
		t->waiting = false;
		u32 chunk = (u32)(len - done) < room ? (u32)(len - done) : room;
		// drop history that is about to be overwritten
		int64_t new_end = s->end + chunk;
		if (new_end - s->base > s->cap) s->base = new_end - s->cap;
		for (u32 copied = 0; copied < chunk;)
		{
			u32 at = (u32)(s->end % s->cap);
			u32 run = s->cap - at;
			if (run > chunk - copied) run = chunk - copied;
			memcpy(s->buf + at, data + done + copied, run);
			s->end += run;
			copied += run;
		}
		LightLock_Unlock(&s->lock);
		done += chunk;
		t->at += chunk;
	}
	return len;
}

// Lets a transfer stop within about a second when the stream is closed or the app exits,
// even while curl is just sitting there waiting on the network.
static int on_progress(void* user, curl_off_t dl_total, curl_off_t dl_now, curl_off_t ul_total, curl_off_t ul_now)
{
	Transfer* t = (Transfer*)user;
	return (t->s->quit || t->gen != t->s->gen || yt_aborting()) ? 1 : 0;
}

static void download_main(void* arg)
{
	HStream* s = (HStream*)arg;
	CURL* curl = curl_easy_init();
	if (!curl)
	{
		LightLock_Lock(&s->lock);
		snprintf(s->error, sizeof(s->error), "Out of memory for the network.");
		LightLock_Unlock(&s->lock);
		return;
	}
	int failures = 0;
	u32 failures_gen = 0;
	while (true)
	{
		LightLock_Lock(&s->lock);
		if (s->quit)
		{
			LightLock_Unlock(&s->lock);
			break;
		}
		if (s->gen != failures_gen)
		{
			failures_gen = s->gen;   // a seek started a new download, which gets its own retries
			failures = 0;
		}
		if (yt_aborting() && !s->error[0])
			snprintf(s->error, sizeof(s->error), "Stopped.");   // the app is closing, so don't retry
		bool idle = s->eof || s->error[0] || (s->size >= 0 && s->end >= s->size);
		if (s->size >= 0 && s->end >= s->size) s->eof = true;
		Transfer t = { .s = s, .gen = s->gen, .at = s->end };
		int64_t size = s->size;
		LightLock_Unlock(&s->lock);
		if (idle)
		{
			svcSleepThread(5 * 1000000ULL);   // wait for a seek or close
			continue;
		}

		int64_t last = t.at + CHUNK_BYTES - 1;
		if (size >= 0 && last >= size) last = size - 1;
		char range[64];
		snprintf(range, sizeof(range), "%lld-%lld", (long long)t.at, (long long)last);

		curl_easy_reset(curl);
		yt_setup_curl(curl);
		curl_easy_setopt(curl, CURLOPT_URL, s->url);
		curl_easy_setopt(curl, CURLOPT_RANGE, range);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &t);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
		curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 32768L);
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);   // reconnect if under 1 KB/s for 15 s
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
		int64_t started_at = t.at;
		t.start_ms = osGetTime();
		CURLcode rc = curl_easy_perform(curl);
		{
			// Log slow or failed requests to help explain playback stalls. Only the network's
			// time counts; time spent waiting for room in the buffer is subtracted.
			u64 took = osGetTime() - t.start_ms - t.wait_ms;
			long got = (long)(t.at - started_at);
			if (rc != CURLE_OK || took > 2000)
				tlog("hstream %c: range %s: %lld KB in %llu ms (first byte after %llu ms), curl %d, HTTP %ld",
				     strstr(s->url, "mime=audio") ? 'a' : 'v', range, (long long)(got / 1024), (unsigned long long)took,
				     (unsigned long long)t.first_ms, (int)rc, t.status);
		}

		LightLock_Lock(&s->lock);
		bool stale = s->quit || t.gen != s->gen;
		if (!stale)
		{
			if (rc == CURLE_OK && (t.status == 206 || t.status == 200))
			{
				failures = 0;
				// A whole-file response (a 200, or a server that ignored Range) or an empty
				// range (we asked past the end) means there's nothing left to fetch.
				if (t.status == 200 || (!t.range_ok && t.at > started_at) || t.at == started_at) s->eof = true;
			}
			else if (t.got_status && (t.status == 403 || t.status == 404 || t.status == 410))
			{
				snprintf(s->error, sizeof(s->error), "The video link expired or was refused (HTTP %ld).", t.status);
				s->err_status = (int)t.status;
			}
			else if (t.status == 416) s->eof = true;   // range starts past the end
			else if (t.at == started_at && ++failures > MAX_RETRIES)
				snprintf(s->error, sizeof(s->error), "Network error: %s", curl_easy_strerror(rc));
			else if (t.at > started_at) failures = 0;   // it made progress before dropping, so just resume
		}
		LightLock_Unlock(&s->lock);
		if (!stale && failures > 0) svcSleepThread(500 * 1000000ULL);
	}
	curl_easy_cleanup(curl);
}

// ---------------------------------------------------------------- public

HStream* hs_open(const char* url, u32 buf_bytes)
{
	HStream* s = (HStream*)calloc(1, sizeof(HStream));
	if (!s) return NULL;
	s->url = strdup(url);
	s->buf = (u8*)malloc(buf_bytes);
	if (!s->url || !s->buf)
	{
		free(s->url);
		free(s->buf);
		free(s);
		return NULL;
	}
	s->cap = buf_bytes;
	s->size = -1;
	LightLock_Init(&s->lock);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// On the New 3DS's extra core the thread can outrank its caller. If that core isn't
	// available it shares the main core, and then it has to rank below the playback loop,
	// otherwise TLS and buffer copies preempt playback and frames get dropped.
	s->core = 2;
	s->thread = threadCreate(download_main, s, STACK_BYTES, prio - 1, 2, false);
	if (!s->thread)
	{
		s->core = -1;
		s->thread = threadCreate(download_main, s, STACK_BYTES, prio + 1, -1, false);
	}
	if (!s->thread)
	{
		free(s->url);
		free(s->buf);
		free(s);
		return NULL;
	}
	return s;
}

int hs_read(HStream* s, u8* out, int len, volatile bool* stop)
{
	while (true)
	{
		LightLock_Lock(&s->lock);
		if (s->pos < s->end)
		{
			int64_t avail = s->end - s->pos;
			int n = avail < len ? (int)avail : len;
			for (int copied = 0; copied < n;)
			{
				u32 at = (u32)(s->pos % s->cap);
				u32 run = s->cap - at;
				if (run > (u32)(n - copied)) run = (u32)(n - copied);
				memcpy(out + copied, s->buf + at, run);
				s->pos += run;
				copied += run;
			}
			LightLock_Unlock(&s->lock);
			return n;
		}
		bool eof = s->eof || (s->size >= 0 && s->pos >= s->size);
		bool err = s->error[0] != 0;
		LightLock_Unlock(&s->lock);
		if (eof) return 0;
		if (err || (stop && *stop)) return -1;
		svcSleepThread(2 * 1000000ULL);
	}
}

int64_t hs_seek(HStream* s, int64_t offset)
{
	if (offset < 0) return -1;
	LightLock_Lock(&s->lock);
	if (s->size >= 0 && offset > s->size) offset = s->size;
	if (offset >= s->base && offset <= s->end) s->pos = offset;   // already buffered, no network needed
	else
	{
		s->base = s->pos = s->end = offset;
		s->gen++;
		s->eof = false;
		// The new download starts with a clean slate. If the problem is still there (an
		// expired link, no network) it will report the error again.
		s->error[0] = 0;
		s->err_status = 0;
	}
	LightLock_Unlock(&s->lock);
	return offset;
}

int64_t hs_tell(HStream* s)
{
	LightLock_Lock(&s->lock);
	int64_t pos = s->pos;
	LightLock_Unlock(&s->lock);
	return pos;
}

int64_t hs_size(HStream* s)
{
	LightLock_Lock(&s->lock);
	int64_t size = s->size;
	LightLock_Unlock(&s->lock);
	return size;
}

bool hs_error(HStream* s, char* out, size_t len)
{
	LightLock_Lock(&s->lock);
	bool err = s->error[0] != 0;
	if (out && len) snprintf(out, len, "%s", s->error);
	LightLock_Unlock(&s->lock);
	return err;
}

int hs_core(HStream* s) { return s->core; }

int hs_http_status(HStream* s)
{
	LightLock_Lock(&s->lock);
	int status = s->err_status;
	LightLock_Unlock(&s->lock);
	return status;
}

void hs_close(HStream* s)
{
	if (!s) return;
	LightLock_Lock(&s->lock);
	s->quit = true;
	LightLock_Unlock(&s->lock);
	threadJoin(s->thread, U64_MAX);
	threadFree(s->thread);
	free(s->url);
	free(s->buf);
	free(s);
}
