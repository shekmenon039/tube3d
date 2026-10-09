#include "subs.h"

#include <stdlib.h>
#include <string.h>

// First occurrence of needle in [p, end), or NULL
static const char* find(const char* p, const char* end, const char* needle)
{
	size_t needle_len = strlen(needle);
	for (; p + needle_len <= end; p++)
		if (*p == needle[0] && memcmp(p, needle, needle_len) == 0) return p;
	return NULL;
}

// Value of the numeric attribute name="123" inside the tag [tag, gt)
static bool attr_u32(const char* tag, const char* gt, const char* name, uint32_t* out)
{
	size_t name_len = strlen(name);
	for (const char* q = tag + 1; q + name_len + 2 < gt; q++)
	{
		if (q[-1] != ' ' || memcmp(q, name, name_len) != 0 || q[name_len] != '=' || q[name_len + 1] != '"') continue;
		uint32_t value = 0;
		const char* digit = q + name_len + 2;
		if (digit >= gt || *digit < '0' || *digit > '9') return false;
		for (; digit < gt && *digit >= '0' && *digit <= '9'; digit++) value = value * 10 + (uint32_t)(*digit - '0');
		*out = value;
		return true;
	}
	return false;
}

static char* put_utf8(char* o, uint32_t c)
{
	if (c < 0x80) *o++ = (char)c;
	else if (c < 0x800)
	{
		*o++ = (char)(0xC0 | (c >> 6));
		*o++ = (char)(0x80 | (c & 0x3F));
	}
	else if (c < 0x10000)
	{
		*o++ = (char)(0xE0 | (c >> 12));
		*o++ = (char)(0x80 | ((c >> 6) & 0x3F));
		*o++ = (char)(0x80 | (c & 0x3F));
	}
	else
	{
		*o++ = (char)(0xF0 | (c >> 18));
		*o++ = (char)(0x80 | ((c >> 12) & 0x3F));
		*o++ = (char)(0x80 | ((c >> 6) & 0x3F));
		*o++ = (char)(0x80 | (c & 0x3F));
	}
	return o;
}

// Decodes the entity starting at p (which points at '&'), writes it to *o and returns the
// position after it. An unknown entity comes out as a plain '&'. It never writes more bytes
// than it consumes, so decoding can happen in place.
static const char* entity(const char* p, const char* end, char** o)
{
	static const struct {
		const char* name;
		char c;
	} named[] = {
		{ "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' },
	};
	for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++)
	{
		size_t name_len = strlen(named[i].name);
		if ((size_t)(end - p) >= name_len && memcmp(p, named[i].name, name_len) == 0)
		{
			*(*o)++ = named[i].c;
			return p + name_len;
		}
	}
	if (end - p >= 4 && p[1] == '#')
	{
		const char* q = p + 2;
		bool hex = *q == 'x' || *q == 'X';
		if (hex) q++;
		uint32_t c = 0;
		const char* digits = q;
		for (; q < end && q - digits < 8; q++)
		{
			int digit = *q >= '0' && *q <= '9'          ? *q - '0'
			            : hex && *q >= 'a' && *q <= 'f' ? *q - 'a' + 10
			            : hex && *q >= 'A' && *q <= 'F' ? *q - 'A' + 10
			                                            : -1;
			if (digit < 0) break;
			c = c * (hex ? 16u : 10u) + (uint32_t)digit;
		}
		// "&#N;" is at least 4 bytes, and a code point that needs k UTF-8 bytes is written with
		// at least k + 3, so the output never outgrows the input
		if (q > digits && q < end && *q == ';' && c > 0 && c <= 0x10FFFF && !(c >= 0xD800 && c <= 0xDFFF))
		{
			*o = put_utf8(*o, c == '\t' ? ' ' : c);
			return q + 1;
		}
	}
	*(*o)++ = '&';
	return p + 1;
}

// Strips leading and trailing spaces and line breaks from [start, end) and returns the new end
static char* trim(char* start, char* end)
{
	char* first = start;
	while (first < end && (*first == ' ' || *first == '\n' || *first == '\t')) first++;
	while (end > first && (end[-1] == ' ' || end[-1] == '\n' || end[-1] == '\t')) end--;
	if (first != start) memmove(start, first, (size_t)(end - first));
	return start + (end - first);
}

// Copies the text of one paragraph [p, end) into o, removing tags (<br> becomes a line
// break), decoding entities, dropping CRs and trimming the ends. Writes at most end - p bytes
// and returns the end of the written text.
static char* para_text(const char* p, const char* end, char* o)
{
	char* start = o;
	while (p < end)
	{
		if (*p == '<')
		{
			const char* gt = memchr(p, '>', (size_t)(end - p));
			if (!gt) break;
			if (end - p >= 3 && (p[1] == 'b' || p[1] == 'B') && (p[2] == 'r' || p[2] == 'R')) *o++ = '\n';
			p = gt + 1;
		}
		else if (*p == '&') p = entity(p, end, &o);
		else if (*p == '\r') p++;
		else *o++ = *p++;
	}
	return trim(start, o);
}

static bool finish(Subs* out, char* o);

static int by_start(const void* a, const void* b)
{
	uint32_t x = ((const SubCue*)a)->start, y = ((const SubCue*)b)->start;
	return x < y ? -1 : x > y;
}

bool subs_parse(const char* xml, size_t len, Subs* out)
{
	memset(out, 0, sizeof(*out));
	const char* end = xml + len;
	int max_cues = 0;
	for (const char* p = xml; (p = find(p, end, "<p ")) != NULL; p += 3) max_cues++;
	if (!max_cues) return false;
	out->cues = (SubCue*)malloc((size_t)max_cues * sizeof(SubCue));
	out->pool = (char*)malloc(len + 1);
	if (!out->cues || !out->pool)
	{
		subs_free(out);
		return false;
	}

	char* o = out->pool;
	for (const char* p = xml; out->n < max_cues && (p = find(p, end, "<p ")) != NULL;)
	{
		const char* gt = memchr(p, '>', (size_t)(end - p));
		if (!gt) break;
		uint32_t start_ms = 0, duration_ms = 0;
		bool has_start = attr_u32(p, gt, "t", &start_ms), has_duration = attr_u32(p, gt, "d", &duration_ms);
		const char* close = gt[-1] == '/' ? gt : find(gt + 1, end, "</p>");
		if (!close) break;
		char* text = o;
		char* text_end = close > gt ? para_text(gt + 1, close, o) : o;
		p = close + 1;
		if (!has_start || text_end == text) continue;   // nothing to show
		*text_end = 0;
		o = text_end + 1;
		SubCue* cue = &out->cues[out->n++];
		cue->start = start_ms;
		cue->end = has_duration ? start_ms + duration_ms : 0;   // a missing end is filled in by finish()
		cue->text = (uint32_t)(text - out->pool);
	}
	return finish(out, o);
}

// Sorts the cues, gives an end to those without one, and gives back the unused part of the
// pool. o is where the used part of the pool ends.
static bool finish(Subs* out, char* o)
{
	if (!out->n)
	{
		subs_free(out);
		return false;
	}
	SubCue* cues = out->cues;
	int count = out->n;
	qsort(cues, (size_t)count, sizeof(SubCue), by_start);
	for (SubCue* cue = cues; cue < cues + count; cue++)
		if (!cue->end)   // NOLINT(clang-analyzer-security.ArrayBound): n <= max cues allocated (loop guard above)
		{
			// No duration given: show it until the next cue, but for 5 s at most
			// NOLINTNEXTLINE(clang-analyzer-security.ArrayBound): cue + 1 is checked against the end first
			uint32_t next = cue + 1 < cues + count ? cue[1].start : cue->start + 5000;
			cue->end = next - cue->start > 5000 ? cue->start + 5000 : next;
		}
	size_t used = (size_t)(o - out->pool);   // never 0, since every cue has text
	char* pool = used ? (char*)realloc(out->pool, used) : NULL;
	if (pool) out->pool = pool;
	return true;
}

// ---------------------------------------------------------------- movie subtitle files

// Copies the text of a SubRip/WebVTT cue or an ASS event [p, end) into o, writing at most
// end - p bytes. Tags like <i> and <font ...> are dropped, as are ASS {...} codes and SubRip's
// {\an8}-style ones. ASS line breaks (\N, \n) and hard spaces (\h) are converted, WebVTT
// entities decoded, CRs dropped and the ends trimmed.
static char* plain_text(const char* p, const char* end, char* o, bool ass)
{
	char* start = o;
	while (p < end)
	{
		if (*p == '{' && (ass || (end - p > 1 && p[1] == '\\')))
		{
			const char* cb = memchr(p, '}', (size_t)(end - p));
			if (cb)
			{
				p = cb + 1;
				continue;
			}
		}
		if (ass && *p == '\\' && end - p > 1 && (p[1] == 'N' || p[1] == 'n' || p[1] == 'h'))
		{
			*o++ = p[1] == 'h' ? ' ' : '\n';
			p += 2;
			continue;
		}
		if (!ass && *p == '<')
		{
			const char* gt = memchr(p, '>', (size_t)(end - p));
			if (gt)
			{
				if (end - p >= 3 && (p[1] == 'b' || p[1] == 'B') && (p[2] == 'r' || p[2] == 'R')) *o++ = '\n';
				p = gt + 1;
				continue;
			}
		}
		if (!ass && *p == '&')
		{
			p = entity(p, end, &o);
			continue;
		}
		if (*p == '\r')
		{
			p++;
			continue;
		}
		*o++ = *p++;
	}
	return trim(start, o);
}

static bool valid_utf8(const unsigned char* s, size_t n)
{
	for (size_t i = 0; i < n;)
	{
		const unsigned char c = s[i];
		size_t k;
		if (c < 0x80) k = 0;
		else if ((c & 0xE0) == 0xC0 && c >= 0xC2) k = 1;
		else if ((c & 0xF0) == 0xE0) k = 2;
		else if ((c & 0xF8) == 0xF0 && c <= 0xF4) k = 3;
		else return false;
		if (i + k >= n) return false;
		for (size_t j = 1; j <= k; j++)
			if ((s[i + j] & 0xC0) != 0x80) return false;
		i += k + 1;
	}
	return true;
}

// Converts the file's text to UTF-8: a malloc'd, NUL-terminated string of *out_len bytes, or
// NULL if memory runs out
static char* to_utf8(const char* data, size_t len, size_t* out_len)
{
	static const uint16_t cp1252[32] = {
		0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
		0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
		0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
	};
	const unsigned char* u = (const unsigned char*)data;
	char* out = (char*)malloc(len * 3 + 8);   // at most 3 bytes per input byte, plus the NUL
	if (!out) return NULL;
	char* o = out;
	if (len >= 2 && ((u[0] == 0xFF && u[1] == 0xFE) || (u[0] == 0xFE && u[1] == 0xFF)))
	{
		// UTF-16 with a byte order mark
		const bool le = u[0] == 0xFF;
		for (size_t i = 2; i + 1 < len; i += 2)
		{
			uint32_t c = le ? (uint32_t)(u[i] | u[i + 1] << 8) : (uint32_t)(u[i] << 8 | u[i + 1]);
			if (c >= 0xD800 && c < 0xDC00 && i + 3 < len)
			{
				const uint32_t c2 = le ? (uint32_t)(u[i + 2] | u[i + 3] << 8) : (uint32_t)(u[i + 2] << 8 | u[i + 3]);
				if (c2 >= 0xDC00 && c2 < 0xE000)
				{
					c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
					i += 2;
				}
			}
			if (c >= 0xD800 && c < 0xE000) c = '?';
			if (c) o = put_utf8(o, c);
		}
	}
	else
	{
		if (len >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF)
		{
			u += 3;
			len -= 3;
		}
		if (valid_utf8(u, len))
		{
			memcpy(o, u, len);
			o += len;
		}
		else
			for (size_t i = 0; i < len; i++)   // Windows-1252: Latin-1 plus curly quotes, dashes and so on
			{
				const uint32_t c = u[i] >= 0x80 && u[i] < 0xA0 ? cp1252[u[i] - 0x80] : u[i];
				o = put_utf8(o, c ? c : '?');
			}
	}
	*o = 0;
	*out_len = (size_t)(o - out);
	return out;
}

// Reads a time such as 01:02:03,456, 02:03.456 or 01:02:03.4 at *pos, skipping leading spaces
static bool parse_time(const char** pos, const char* end, uint32_t* ms)
{
	const char* p = *pos;
	while (p < end && (*p == ' ' || *p == '\t')) p++;
	uint32_t fields[3];
	int count = 0;
	for (;;)
	{
		if (p >= end || *p < '0' || *p > '9' || count == 3) return false;
		uint32_t value = 0;
		for (int digits = 0; p < end && *p >= '0' && *p <= '9'; p++, digits++)
			if (digits < 6) value = value * 10 + (uint32_t)(*p - '0');
		fields[count++] = value;
		if (p < end && *p == ':')
		{
			p++;
			continue;
		}
		break;
	}
	if (count < 2) return false;
	uint32_t total_ms =
	    (count == 3 ? fields[0] * 3600 + fields[1] * 60 + fields[2] : fields[0] * 60 + fields[1]) * 1000;
	if (p < end && (*p == ',' || *p == '.'))
	{
		p++;
		uint32_t scale = 100;
		for (; p < end && *p >= '0' && *p <= '9'; p++, scale /= 10) total_ms += (uint32_t)(*p - '0') * scale;
	}
	*ms = total_ms;
	*pos = p;
	return true;
}

// The end of the line at p: its '\n', or end if there is none
static const char* line_end(const char* p, const char* end)
{
	const char* nl = memchr(p, '\n', (size_t)(end - p));
	return nl ? nl : end;
}

static bool blank_line(const char* p, const char* stop)
{
	for (; p < stop; p++)
		if (*p != ' ' && *p != '\t' && *p != '\r') return false;
	return true;
}

bool subs_parse_file(const char* data, size_t len, Subs* out)
{
	memset(out, 0, sizeof(*out));
	size_t text_len = 0;
	char* buf = to_utf8(data, len, &text_len);
	if (!buf) return false;
	const char* end = buf + text_len;
	const char* first = buf;
	while (first < end && (*first == ' ' || *first == '\r' || *first == '\n' || *first == '\t')) first++;
	if (first < end && *first == '<')
	{
		const bool ok = subs_parse(buf, text_len, out);   // srv3 XML
		free(buf);
		return ok;
	}
	int max_cues = 0;
	for (const char* p = buf; (p = find(p, end, "-->")) != NULL; p += 3) max_cues++;
	if (!max_cues)
	{
		free(buf);
		return false;
	}
	out->cues = (SubCue*)malloc((size_t)max_cues * sizeof(SubCue));
	out->pool = (char*)malloc(text_len + (size_t)max_cues + 1);
	if (!out->cues || !out->pool)
	{
		subs_free(out);
		free(buf);
		return false;
	}
	char* o = out->pool;
	for (const char* p = buf; p < end && out->n < max_cues;)
	{
		const char* line_stop = line_end(p, end);
		const char* arrow = find(p, line_stop, "-->");
		uint32_t start_ms = 0, end_ms = 0;
		const char* start_pos = p;
		const char* end_pos = arrow ? arrow + 3 : NULL;
		if (!arrow || !parse_time(&start_pos, arrow, &start_ms) || !parse_time(&end_pos, line_stop, &end_ms))
		{
			p = line_stop < end ? line_stop + 1 : end;
			continue;
		}
		// The cue's text runs until a blank line or the next timing line
		const char* body = line_stop < end ? line_stop + 1 : end;
		const char* body_end = body;
		const char* last_line = body;
		p = body;
		while (p < end)
		{
			const char* eol = line_end(p, end);
			if (blank_line(p, eol) || find(p, eol, "-->")) break;
			last_line = p;
			body_end = eol;
			p = eol < end ? eol + 1 : end;
		}
		if (p < end && find(p, line_end(p, end), "-->") && last_line < body_end)
		{
			// No blank line before the next cue, so the last line is probably its number, not text
			const char* scan = last_line;
			while (scan < body_end && ((*scan >= '0' && *scan <= '9') || *scan == '\r' || *scan == ' ')) scan++;
			if (scan == body_end) body_end = last_line;
		}
		char* text = o;
		char* text_end = plain_text(body, body_end, o, false);
		if (text_end == text) continue;
		*text_end = 0;
		o = text_end + 1;
		SubCue* cue = &out->cues[out->n++];
		cue->start = start_ms;
		cue->end = end_ms > start_ms ? end_ms : 0;
		cue->text = (uint32_t)(text - out->pool);
	}
	free(buf);
	return finish(out, o);
}

// Subtitle tracks inside a file grow while it plays, so each one is capped. A 3-hour film has
// around 2000 cues; the limits are only there to stop a broken or hostile file eating memory.
#define SUBS_MAX_CUES 20000
#define SUBS_MAX_POOL (4u * 1024 * 1024)

bool subs_add(Subs* s, uint32_t start, uint32_t end, const char* text, size_t len, bool ass)
{
	if (!text || !len || len > 64 * 1024) return true;
	if (s->n >= SUBS_MAX_CUES || s->pool_used >= SUBS_MAX_POOL) return true;   // full, so drop the rest
	const char* p = text;
	const char* stop = text + len;
	if (ass)
	{
		// The text follows the 8th comma, or the 9th in a full script's "Dialogue:" lines
		const int commas = len > 9 && !memcmp(text, "Dialogue:", 9) ? 9 : 8;
		const char* q = p;
		for (int i = 0; i < commas && q; i++)
		{
			q = memchr(q, ',', (size_t)(stop - q));
			if (q) q++;
		}
		if (q) p = q;
	}
	char* plain = (char*)malloc((size_t)(stop - p) + 1);
	if (!plain) return false;
	char* plain_end = plain_text(p, stop, plain, ass);
	*plain_end = 0;
	const uint32_t plain_len = (uint32_t)(plain_end - plain);
	if (!plain_len)
	{
		free(plain);
		return true;
	}
	// Insert after the cues that start at or before it, unless the same cue is already there
	int lo = 0, hi = s->n;
	while (lo < hi)
	{
		const int mid = (lo + hi) / 2;
		if (s->cues[mid].start <= start) lo = mid + 1;
		else hi = mid;
	}
	for (int i = lo - 1; i >= 0 && s->cues[i].start == start; i--)
		if (!strcmp(s->pool + s->cues[i].text, plain))
		{
			free(plain);
			return true;
		}
	if (s->n == s->cap)
	{
		const int cap = s->cap ? s->cap * 2 : 64;
		SubCue* c = (SubCue*)realloc(s->cues, (size_t)cap * sizeof(SubCue));
		if (!c)
		{
			free(plain);
			return false;
		}
		s->cues = c;
		s->cap = cap;
	}
	if (s->pool_used + plain_len + 1 > s->pool_cap)
	{
		uint32_t cap = s->pool_cap ? s->pool_cap * 2 : 4096;
		if (cap < s->pool_used + plain_len + 1) cap = s->pool_used + plain_len + 1;
		char* pool = (char*)realloc(s->pool, cap);
		if (!pool)
		{
			free(plain);
			return false;
		}
		s->pool = pool;
		s->pool_cap = cap;
	}
	memcpy(s->pool + s->pool_used, plain, plain_len + 1);
	free(plain);
	memmove(&s->cues[lo + 1], &s->cues[lo], (size_t)(s->n - lo) * sizeof(SubCue));
	s->cues[lo].start = start;
	s->cues[lo].end = end > start ? end : start + 4000;
	s->cues[lo].text = s->pool_used;
	s->pool_used += plain_len + 1;
	s->n++;
	return true;
}

void subs_free(Subs* s)
{
	free(s->cues);
	free(s->pool);
	memset(s, 0, sizeof(*s));
}

const char* subs_at(const Subs* s, uint32_t ms, char* out, size_t cap)
{
	if (!cap) return out;
	out[0] = 0;
	if (!s || !s->n) return out;
	// last cue starting at or before ms
	int lo = 0, hi = s->n - 1, last = -1;
	while (lo <= hi)
	{
		int mid = (lo + hi) / 2;
		if (s->cues[mid].start <= ms)
		{
			last = mid;
			lo = mid + 1;
		}
		else hi = mid - 1;
	}
	// Take the newest two active cues. A long earlier one, like "[Music]", may still be up.
	int shown[2], shown_count = 0;
	for (int i = last; i >= 0 && i > last - 16 && shown_count < 2; i--)
		if (s->cues[i].end > ms) shown[shown_count++] = i;
	size_t used = 0;
	for (int j = shown_count - 1; j >= 0; j--)
	{
		const char* text = s->pool + s->cues[shown[j]].text;
		size_t len = strlen(text);
		if (used && used + 1 < cap) out[used++] = '\n';
		if (used + len >= cap) len = cap - 1 - used;
		memcpy(out + used, text, len);
		used += len;
	}
	out[used] = 0;
	// If the text was cut off in the middle of a UTF-8 character, drop that character
	size_t i = used;
	while (i > 0 && ((unsigned char)out[i - 1] & 0xC0) == 0x80) i--;
	if (i > 0 && (unsigned char)out[i - 1] >= 0xC0)
	{
		unsigned char lead = (unsigned char)out[i - 1];
		size_t want = (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : 4;
		if (used - (i - 1) < want) out[i - 1] = 0;
	}
	return out;
}
