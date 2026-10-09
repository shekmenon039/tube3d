#pragma once
// Talks to YouTube's InnerTube API over HTTPS for search, stream URLs, captions and comments.
// It's written in C++ (yt.cpp) behind a C interface so the rest of the app can stay C.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	char id[16];
	int duration;   // seconds; 0 for live or unknown
	char channel[64];
	char title[256];   // UTF-8
	char views[24];    // YouTube's short form, e.g. "1.2M views", or "" if not given
} YtItem;

// The widest DASH video yt_get_streams will pick, because the player's video texture is
// 1024 px wide. A format that doesn't state its width is assumed to be 16:9.
#define YT_MAX_VIDEO_WIDTH 1024

enum { YT_STEREO_NONE, YT_STEREO_SBS, YT_STEREO_TB };   // YtStreams.stereo; the left or top half is the left eye

// A subtitle track YouTube offers for the video (from captions.playerCaptionsTracklistRenderer).
// url returns the track as srv3 XML (see subs.h), and with the ANDROID client it needs no PO
// token. Auto-translated tracks (&tlang=) aren't offered because YouTube answers them with 429.
#define YT_MAX_CAPTIONS 24
typedef struct {
	char lang[16];         // e.g. "en", "pt-BR"
	char name[64];         // UTF-8, e.g. "English (auto-generated)"
	bool auto_generated;   // kind "asr": speech recognition, shown as rolling lines
	char* url;
} YtCaption;

typedef struct {
	char title[256];
	char author[64];
	char views[24];   // view count in YouTube's short form ("1.8B views"), "" if unknown
	uint32_t duration_ms;
	bool is_live;                          // a live stream; muxed_url is then "live:..." (see ytsrc.h, livesrc.h)
	int video_height;                      // height of video_url, 0 if there is none
	int video_width;                       // width of video_url (<= YT_MAX_VIDEO_WIDTH), 0 if there is none
	char* video_url;                       // H.264 video-only DASH stream, or NULL. video_url and audio_url
	                                       // are either both set or both NULL.
	char* audio_url;                       // AAC audio-only DASH stream (itag 140 preferred), or NULL
	char* muxed_url;                       // 360p H.264 + AAC in one file (itag 18), or NULL
	bool is_360;                           // a 360 or VR180 video. We go by a format's projectionType being
	                                       // EQUIRECTANGULAR or MESH; the muxed 360p format says RECTANGULAR
	                                       // even for these.
	int stereo;                            // the 3D layout of video_url as YouTube states it (YT_STEREO_*).
	                                       // 0 means flat or simply not stated, and most SBS/TB uploads aren't
	                                       // marked. Only the VISIONOS client is told (stereoLayout); other
	                                       // clients get a flat one-eye version of a marked 3D upload.
	bool dash_cut_off;                     // Without a PO token YouTube can stop serving the separate streams
	                                       // part-way, which we detect by a byte near the end of the video or
	                                       // audio being refused. video_url/audio_url are then dropped and
	                                       // muxed_url should be played. They're also dropped, without this
	                                       // flag, when the stream size is unknown and can't be estimated, or
	                                       // when no DASH video fits the size limits.
	YtCaption captions[YT_MAX_CAPTIONS];   // the uploader's tracks first, then auto-generated ones
	int caption_count;
	char error[200];   // empty on success
} YtStreams;

// Call yt_init once networking is up, and yt_exit before it goes down.
bool yt_init(void);
void yt_exit(void);

// Searches YouTube, reading several result pages. Shorts are left out. With three_d it looks
// for side-by-side and top-bottom uploads: unless the query already says 3D it searches for
// "<query> 3D SBS" and "<query> 3D side by side", drops 360°, VR180 and anaglyph videos, and
// lists titles that name a layout first. Returns how many videos were written to out, or -1
// with err set.
// If more isn't NULL, *more receives a cursor for the following pages (NULL if YouTube has
// none). yt_search_more then writes up to max further videos to out, skipping any the cursor
// has already returned, and returns how many, 0 once the pages run out, or -1 with err set.
// Use a cursor from one thread at a time and free it with yt_search_free (NULL is fine).
typedef struct YtSearchCursor YtSearchCursor;
int yt_search_paged(const char* query, bool three_d, YtItem* out, int max, YtSearchCursor** more, char* err,
                    size_t errlen);
int yt_search_more(YtSearchCursor* c, YtItem* out, int max, char* err, size_t errlen);
void yt_search_free(YtSearchCursor* c);

// Looks up the playable streams of a video. max_height caps the video stream's height (e.g.
// 480) and YT_MAX_VIDEO_WIDTH its width. Returns false with out->error set if the video can't
// be played. Free the result with yt_free_streams.
// It keeps no per-video state, so it's safe to call from any thread and to call again for
// the same video, for instance when a stream URL expired or was refused mid-play with HTTP
// 403/410; you get fresh URLs. out is overwritten without being freed, so call
// yt_free_streams on it first.
bool yt_get_streams(const char* video_id, int max_height, YtStreams* out);
void yt_free_streams(YtStreams* s);

// Makes a yt_get_streams running on another thread give up within about a second and return
// false with the error "Cancelled" (the user pressed Cancel). Searches and thumbnail downloads
// carry on. The flag is cleared when the next yt_get_streams starts, so a cancel that arrives
// before the lookup has begun gets lost. While waiting for the lookup thread, keep calling
// this until the thread has finished; it's cheap and idempotent.
void yt_cancel_streams(void);

// A video's comments (read only), in YouTube's "Top comments" order, about 20 per page.
#define YT_COMMENT_TEXT 640
typedef struct {
	char author[48];   // "@name"
	char when[32];     // "1 year ago" (or "3 days ago (edited)")
	char likes[16];    // "324K", or "" when there are none
	int replies;
	bool by_uploader;   // written by the video's own channel
	bool pinned;
	char text[YT_COMMENT_TEXT];   // UTF-8 with '\n' between lines, cut short if too long
} YtComment;

// yt_comments fetches the first page and returns how many comments it wrote to out (0 if
// there are none yet). *more receives a cursor for the next pages, or NULL if there are none.
// On failure it returns -1 with err set; a video with comments turned off gives -1 and
// "Comments are turned off.". yt_comments_more fetches the next page (0 when there's no more).
// yt_cancel_comments makes a running request give up, for when the player is leaving. Use a
// cursor from one thread at a time and free it with yt_comments_free (NULL is fine).
typedef struct YtCommentsCursor YtCommentsCursor;
int yt_comments(const char* video_id, YtComment* out, int max, YtCommentsCursor** more, char* err, size_t errlen);
int yt_comments_more(YtCommentsCursor* c, YtComment* out, int max, char* err, size_t errlen);
void yt_comments_free(YtCommentsCursor* c);
void yt_cancel_comments(void);
void yt_comments_allow(void);   // call before starting a comments thread to undo an earlier cancel

// A video's like and dislike counts in short form ("2.6M", "101K"). Likes come from YouTube;
// dislikes are Return YouTube Dislike's estimate, since YouTube no longer publishes them. Either
// is "" when unknown; returns false if neither could be found. It uses the comments
// connection (taking turns with yt_comments), so yt_cancel_comments stops it too.
bool yt_votes(const char* video_id, char* likes, size_t likes_len, char* dislikes, size_t dislikes_len);

// Applies Tube3D's TLS policy (Google's root certificates, with chain and hostname checks),
// user agent and connect timeout to a curl easy handle. The handle is a void* to keep curl
// out of this header. It also installs a progress callback that aborts the transfer once
// yt_abort() has been called.
void yt_setup_curl(void* curl_easy_handle);

// Makes every running and future transfer fail within about a second, for when the app closes.
void yt_abort(void);
bool yt_aborting(void);

// Small HTTPS GETs such as thumbnails. Use one handle per thread so its connection gets
// reused. yt_http_get returns the HTTP status or -1, and caps the body at cap bytes.
void* yt_http_new(void);
int yt_http_get(void* handle, const char* url, unsigned char* buf, size_t cap, size_t* len);
// The same, but accepts gzip and gives up within about a second once *cancel becomes true.
int yt_http_get_ex(void* handle, const char* url, unsigned char* buf, size_t cap, size_t* len, volatile bool* cancel);
void yt_http_free(void* handle);

#ifdef __cplusplus
}
#endif
