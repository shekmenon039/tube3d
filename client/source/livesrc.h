#pragma once
// Presents a YouTube live stream to the MPEG-TS demuxer as one endless byte stream. A
// background thread re-reads the HLS media playlist every few seconds and appends each new
// chunk (about 5 s of MPEG-TS) to a fixed buffer. Playback starts a few chunks behind the live
// edge. If a long stall pushes us out of YouTube's window, we jump back to the edge; the
// timestamps jump with it, and the source smooths that over.
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct LiveSrc LiveSrc;

// Starts following the media playlist at url, buffering up to buf_bytes. NULL if out of memory.
LiveSrc* ls_open(const char* url, u32 buf_bytes);

// Reads up to len bytes, blocking until some arrive. Returns 0 once the broadcast has
// finished, and -1 on an error or when *stop becomes true.
int ls_read(LiveSrc* s, u8* buf, int len, volatile bool* stop);

// True after a fatal error, such as the playlist being refused or gone, or no new chunks for
// a long time. The message is copied into out, which may be NULL.
// ls_http_status returns 403, 404 or 410 when YouTube refused the playlist (ask
// yt_get_streams for a fresh one), and 0 otherwise.
bool ls_error(LiveSrc* s, char* out, size_t len);
int ls_http_status(LiveSrc* s);

void ls_close(LiveSrc* s);
