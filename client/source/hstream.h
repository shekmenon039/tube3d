#pragma once
// Streams one HTTP(S) URL through a fixed-size buffer, with a background thread doing the
// downloading. This is the byte source the MP4 demuxer reads YouTube videos from.
//
// The thread fetches 1 MB Range requests over a single kept-alive connection. The buffer
// never grows; the download simply pauses while it's full. Seeking within the buffered window
// (or up to 64 KB behind it) is free, while any other seek reconnects at the new offset. A
// dropped connection picks up where it left off.
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct HStream HStream;

// Starts downloading url from offset 0 into a buffer of buf_bytes. NULL if out of memory.
HStream* hs_open(const char* url, u32 buf_bytes);

// Reads up to len bytes at the current position. Blocks until data arrives.
// Returns bytes read, 0 at the end of the stream, -1 on error or when *stop becomes true.
int hs_read(HStream* s, u8* buf, int len, volatile bool* stop);

// Moves the read position. Returns the new position or -1.
int64_t hs_seek(HStream* s, int64_t offset);

int64_t hs_tell(HStream* s);
int64_t hs_size(HStream* s);   // total length once known, else -1

// True after a fatal error: the link was refused or expired, the network stayed down after
// retries, or the app is closing. The message is copied into out, which may be NULL. A seek
// that starts a new download clears the error, and the download sets it again if the cause
// is still there.
bool hs_error(HStream* s, char* out, size_t len);

// The HTTP status behind the current fatal error. It's 403, 404 or 410 when the server
// refused the link because it expired or was cut off; the caller should then get fresh URLs
// from yt_get_streams or fall back to the muxed stream. 0 for any other error, or none.
int hs_http_status(HStream* s);
int hs_core(HStream* s);   // download thread's core: 2 (the extra core) or -1 (shared with the caller)

void hs_close(HStream* s);
