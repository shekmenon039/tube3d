#pragma once
// Subtitles: parses YouTube's srv3 timed text (and subtitle files) into a compact cue list,
// and works out the text to show at a given time. It's pure C with no 3DS calls, so
// it can be tested on a PC.
//
// srv3 is XML of the form <p t="start ms" d="duration ms">text</p>. The text is either plain,
// with "\n" line breaks, or split into word segments <s ...>word</s> by speech recognition,
// and it may contain XML entities like &amp; or &#39;. Auto-generated tracks overlap their
// paragraphs so that two lines roll up the screen. Showing the newest two active paragraphs
// gives the same look as YouTube.
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
	uint32_t start, end;   // ms
	uint32_t text;         // offset of the cue's NUL-terminated UTF-8 text in Subs.pool
} SubCue;

typedef struct {
	SubCue* cues;   // sorted by start
	int n;
	char* pool;
	int cap;   // used by subs_add: cues allocated, then the pool's used and total size
	uint32_t pool_used, pool_cap;
} Subs;

// Parses srv3 XML of len bytes (it need not be NUL-terminated). Returns false, leaving *out
// empty, if there are no cues or memory runs out. Free the result with subs_free.
bool subs_parse(const char* xml, size_t len, Subs* out);

// Parses a movie's subtitle file: SubRip (.srt), WebVTT (.vtt) or srv3. The text may be UTF-8,
// UTF-16 with a byte order mark, or Windows-1252 when it isn't valid UTF-8 (common in older
// .srt files). Tags like <i> and <font> and codes like {\an8} are dropped.
bool subs_parse_file(const char* data, size_t len, Subs* out);

// Adds one cue as it arrives, for a subtitle track inside a video file. The list stays sorted,
// and a cue with the same start and text as an existing one (seen again after seeking back)
// is skipped. If ass is set the text is an ASS/SSA event
// ("ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text", with {...} codes and \N
// line breaks); otherwise it's SubRip-like text. An end of 0 shows the cue for 4 s.
// Start from a zeroed Subs.
bool subs_add(Subs* s, uint32_t start, uint32_t end, const char* text, size_t len, bool ass);
void subs_free(Subs* s);

// The text to show at ms: the newest two cues active then, older first, joined by '\n'.
// Returns out, which is an empty string when nothing is shown.
const char* subs_at(const Subs* s, uint32_t ms, char* out, size_t cap);
