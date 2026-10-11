// Tube3D's YouTube backend: search, stream URLs, captions and comments through YouTube's
// InnerTube API, using libcurl and mbedTLS.
//
// The request formats and client identities (MWEB for search, and ANDROID for the player
// endpoint because it returns plain stream URLs without signature ciphers) are adapted from
// FourthTube's youtube_parser (https://github.com/erievs/FourthTube, GPL-3.0).
// Where we differ: request bodies are built with a JSON writer so search text is escaped,
// every request has timeouts and a response size cap, TLS is verified against Google's root
// CAs only, visitorData is cached, and a video takes one request instead of four or five.
#include "yt.h"

#include <ctype.h>
#include <math.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <new>
#include <string>
#include <vector>

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#ifdef __3DS__
#include <3ds.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include "yt_roots_bin.h"   // client/data/yt_roots.bin: the Google Trust Services and GlobalSign R4 roots
typedef LightLock YtLock;
static void lock_init(YtLock* l) { LightLock_Init(l); }
static void lock(YtLock* l) { LightLock_Lock(l); }
static void unlock(YtLock* l) { LightLock_Unlock(l); }
#else
#include <pthread.h>
typedef pthread_mutex_t YtLock;
static void lock_init(YtLock* l) { pthread_mutex_init(l, NULL); }
static void lock(YtLock* l) { pthread_mutex_lock(l); }
static void unlock(YtLock* l) { pthread_mutex_unlock(l); }
#endif

using rapidjson::Document;
using rapidjson::Value;

#define MWEB_VERSION "2.20241202.07.00"
#define MAX_RESPONSE (2 * 1024 * 1024)   // player responses are around 0.2-0.6 MB
static const char* USER_AGENT = "Mozilla/5.0 (Linux; Android 11; Pixel 3a) AppleWebKit/537.36 (KHTML, like Gecko) "
                                "Chrome/83.0.4103.101 Mobile Safari/537.36";

static CURL* curl = NULL;   // reused so connections and TLS sessions stay open
static YtLock curl_lock;
// Search and comments run on their own threads with their own handles, so loading a page of
// results or comments never makes the stream lookup (pressing Play) wait behind it.
static CURL* search_curl = NULL;
static YtLock search_lock;
static CURL* comments_curl = NULL;
static YtLock comments_lock;
static std::string visitor_data;   // fetched once per app run

// ---------------------------------------------------------------- HTTP

#ifdef __3DS__
// The 3DS clock is often wrong: never set, reset by a flat battery, or moved for games. That
// would make every certificate look expired or not yet valid. So we ignore the validity dates
// only when the clock is obviously wrong (before 2025 or after 2044), and check them as usual
// otherwise. Chain (NOT_TRUSTED) and hostname (CN_MISMATCH) verification always stay on.
#define CLOCK_PLAUSIBLE_FROM  1735689600LL   // 2025-01-01 00:00 UTC
#define CLOCK_PLAUSIBLE_UNTIL 2366841600LL   // 2045-01-01 00:00 UTC
static bool clock_plausible(void)
{
	long long now = (long long)time(NULL);
	return now >= CLOCK_PLAUSIBLE_FROM && now < CLOCK_PLAUSIBLE_UNTIL;
}
static int verify_dates_if_clock_ok(void*, mbedtls_x509_crt*, int, uint32_t* flags)
{
	if (!clock_plausible()) *flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
	return 0;
}
static CURLcode on_ssl_ctx(CURL*, void* ssl_ctx, void*)
{
	mbedtls_ssl_conf_verify((mbedtls_ssl_config*)ssl_ctx, verify_dates_if_clock_ok, NULL);
	return CURLE_OK;
}
#endif

struct Body {
	std::string data;
	bool overflow = false;
};

static size_t on_data(char* data, size_t size, size_t n, void* user)
{
	Body* body = (Body*)user;
	size_t len = size * n;
	if (body->data.size() + len > MAX_RESPONSE)
	{
		body->overflow = true;
		return 0;   // aborts the transfer
	}
	body->data.append(data, len);
	return len;
}

static volatile bool aborting = false;
// Set by yt_cancel_streams and cleared when the next yt_get_streams starts. Only the
// transfers of yt_get_streams look at it (it's passed as their progress data), so searches
// and thumbnail downloads carry on.
static volatile bool streams_cancel = false;

extern "C" void yt_abort(void) { aborting = true; }
extern "C" bool yt_aborting(void) { return aborting; }
extern "C" void yt_cancel_streams(void) { streams_cancel = true; }
static volatile bool comments_cancel = false;   // set by yt_cancel_comments, cleared by yt_comments_allow
extern "C" void yt_cancel_comments(void) { comments_cancel = true; }
extern "C" void yt_comments_allow(void) { comments_cancel = false; }

// curl calls this during connects, handshakes and transfers, roughly once a second even when
// nothing arrives, and a non-zero return aborts the transfer. user is either NULL or a cancel
// flag that should also abort this transfer.
static int on_progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	return aborting || (user && *(volatile bool*)user) ? 1 : 0;
}

extern "C" void yt_setup_curl(void* handle)
{
	CURL* c = (CURL*)handle;
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
	curl_easy_setopt(c, CURLOPT_XFERINFODATA, NULL);
	curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
	// HTTPS only, redirects included, so a stream URL can't redirect us to http:// or file://
	curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
	curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#if defined(YT_ROOTS_FILE)
	curl_easy_setopt(c, CURLOPT_CAINFO, YT_ROOTS_FILE);   // a PC build
#elif defined(__3DS__)
	struct curl_blob roots = { (void*)yt_roots_bin, yt_roots_bin_size, CURL_BLOB_NOCOPY };
	curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &roots);
	curl_easy_setopt(c, CURLOPT_SSL_CTX_FUNCTION, on_ssl_ctx);
#endif
}

// A GET, or a POST with a JSON body when post_json isn't NULL. Returns the HTTP status, or
// -1 with err set. If cancel is given, the transfer aborts once it becomes true. The caller
// must hold the lock that goes with h.
static long http_on(CURL* h, const std::string& url, const std::string* post_json, Body& out, const char* extra_header,
                    std::string& err, volatile bool* cancel = NULL)
{
	curl_easy_reset(h);
	yt_setup_curl(h);
	curl_easy_setopt(h, CURLOPT_XFERINFODATA, (void*)cancel);
	curl_easy_setopt(h, CURLOPT_URL, url.c_str());
	curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");   // gzip if the server offers it
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_data);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, &out);
	curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, post_json ? 0L : 1L);
	curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 256L);   // give up below 256 B/s ...
	curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 20L);     // ... sustained for 20 s
	curl_easy_setopt(h, CURLOPT_TIMEOUT, 60L);

	struct curl_slist* headers = NULL;
	headers = curl_slist_append(headers, "Accept-Language: en;q=0.9");
	if (extra_header) headers = curl_slist_append(headers, extra_header);
	if (post_json)
	{
		headers = curl_slist_append(headers, "Content-Type: application/json");
		curl_easy_setopt(h, CURLOPT_POSTFIELDS, post_json->c_str());
		curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)post_json->size());
	}
	curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);

	CURLcode rc = curl_easy_perform(h);
	curl_slist_free_all(headers);
	curl_easy_setopt(h, CURLOPT_HTTPHEADER, NULL);
	if (out.overflow)
	{
		err = "The response from YouTube was too large.";
		return -1;
	}
	if (rc != CURLE_OK)
	{
		err = std::string("Network error: ") + curl_easy_strerror(rc);
		return -1;
	}
	long status = 0;
	curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
	return status;
}

// http_on with the stream lookup's handle. The caller holds curl_lock.
static long http(const std::string& url, const std::string* post_json, Body& out, const char* extra_header,
                 std::string& err, volatile bool* cancel = NULL)
{
	return http_on(curl, url, post_json, out, extra_header, err, cancel);
}

static std::string innertube_url(const char* api)
{
	return std::string("https://m.youtube.com/youtubei/v1/") + api + "?prettyPrint=false";   // no API key needed
}

// ---------------------------------------------------------------- JSON helpers

typedef rapidjson::Writer<rapidjson::StringBuffer> JsonWriter;

static void kv(JsonWriter& w, const char* k, const char* v)
{
	w.Key(k);
	w.String(v);
}

// The "context" object of an InnerTube request made as MWEB (search and the watch page).
static void write_mweb_context(JsonWriter& w)
{
	w.Key("context");
	w.StartObject();
	w.Key("client");
	w.StartObject();
	kv(w, "hl", "en");
	kv(w, "gl", "US");
	kv(w, "clientName", "MWEB");
	kv(w, "clientVersion", MWEB_VERSION);
	w.EndObject();
	w.EndObject();
}

static const Value* get(const Value* v, const char* key)
{
	if (!v || !v->IsObject()) return NULL;
	Value::ConstMemberIterator it = v->FindMember(key);
	return it == v->MemberEnd() ? NULL : &it->value;
}

static const Value* path(const Value* v, std::initializer_list<const char*> keys)
{
	for (const char* k : keys) v = get(v, k);
	return v;
}

static std::string str(const Value* v)
{
	return v && v->IsString() ? std::string(v->GetString(), v->GetStringLength()) : "";
}
static int num(const Value* v)
{
	if (!v) return 0;
	if (v->IsInt()) return v->GetInt();
	if (v->IsString()) return (int)strtol(v->GetString(), NULL, 10);   // numbers sent as strings, like "itag": "18"
	return 0;
}

// YouTube text objects come as {"simpleText": "..."} or {"runs": [{"text": "..."}, ...]}.
static std::string text_of(const Value* v)
{
	if (const Value* s = get(v, "simpleText")) return str(s);
	std::string out;
	if (const Value* runs = get(v, "runs"))
		if (runs->IsArray())
			for (const Value& r : runs->GetArray())   // NOLINT(clang-analyzer-core.NullDereference): checked IsArray
				out += str(get(&r, "text"));
	return out;
}

// For text cut to its first n bytes: the length without a last UTF-8 character that lost
// some of its bytes.
static size_t utf8_whole_len(const char* text, size_t n)
{
	size_t i = n;
	while (i > 0 && ((unsigned char)text[i - 1] & 0xC0) == 0x80) i--;
	if (i == 0) return n;
	unsigned char lead = (unsigned char)text[i - 1];
	size_t want = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : 4;
	return n - (i - 1) < want ? i - 1 : n;
}

// Copies text from YouTube into a fixed-size field. Control characters such as tabs and
// newlines become spaces, so a title can't break the tab-separated history file or the UI.
static void copy_str(char* dst, size_t cap, const std::string& s)
{
	snprintf(dst, cap, "%s", s.c_str());
	for (char* c = dst; *c; c++)
		if ((unsigned char)*c < 0x20 || *c == 0x7F) *c = ' ';
	if (s.size() >= cap) dst[utf8_whole_len(dst, strlen(dst))] = 0;
}

// Parses in place and without recursion, so a deeply nested reply can't overflow the 64 KB
// stacks of the threads this runs on.
static bool parse_json(Document& doc, char* text)
{
	return !doc.ParseInsitu<rapidjson::kParseIterativeFlag>(text).HasParseError();
}

static int parse_duration(const std::string& t)   // "1:02:03" gives 3723
{
	int total = 0, part = 0;
	bool any = false;
	for (char c : t)
	{
		if (c >= '0' && c <= '9')
		{
			part = part * 10 + (c - '0');
			any = true;
		}
		else if (c == ':')
		{
			total = total * 60 + part;
			part = 0;
		}
		else if (c != ' ') return 0;
	}
	return any ? total * 60 + part : 0;
}

static std::string url_decode(const std::string& in)
{
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); i++)
	{
		if (in[i] == '%' && i + 2 < in.size() && isxdigit((unsigned char)in[i + 1]) &&
		    isxdigit((unsigned char)in[i + 2]))
		{
			char hex[3] = { in[i + 1], in[i + 2], 0 };
			out.push_back((char)strtol(hex, NULL, 16));
			i += 2;
		}
		else out.push_back(in[i]);
	}
	return out;
}

// Returns url with its query parameter key set to value, replacing any existing one.
static std::string with_param(std::string url, const char* key, const char* value)
{
	std::string prefix = std::string("&") + key + "=";
	size_t start = url.find(prefix);
	if (start != std::string::npos)
	{
		size_t end = url.find('&', start + 1);
		url.erase(start, end == std::string::npos ? std::string::npos : end - start);
	}
	return url + prefix + value;
}

static char* dup_or_null(const std::string& s) { return s.empty() ? NULL : strdup(s.c_str()); }

static size_t discard(char*, size_t size, size_t n, void*) { return size * n; }

// True if the server will hand out the byte at `offset` (answers HTTP 206).
static bool range_ok(const std::string& url, uint64_t offset)
{
	char range[48];
	snprintf(range, sizeof(range), "%llu-%llu", (unsigned long long)offset, (unsigned long long)offset);
	lock(&curl_lock);
	curl_easy_reset(curl);
	yt_setup_curl(curl);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void*)&streams_cancel);   // yt_cancel_streams can stop it
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_RANGE, range);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	CURLcode rc = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	unlock(&curl_lock);
	return rc == CURLE_OK && status == 206;
}

// ---------------------------------------------------------------- public API

extern "C" bool yt_init(void)
{
	if (curl) return true;
	lock_init(&curl_lock);
	lock_init(&search_lock);
	lock_init(&comments_lock);
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return false;
	curl = curl_easy_init();
	search_curl = curl_easy_init();
	comments_curl = curl_easy_init();
	return curl && search_curl && comments_curl;
}

extern "C" void yt_exit(void)
{
	if (!curl) return;
	curl_easy_cleanup(curl);
	curl = NULL;
	if (search_curl) curl_easy_cleanup(search_curl);
	search_curl = NULL;
	if (comments_curl) curl_easy_cleanup(comments_curl);
	comments_curl = NULL;
	curl_global_cleanup();
	visitor_data.clear();
	visitor_data.shrink_to_fit();
}

// Turns a count into YouTube's short form: 101116 becomes "101K" and 2676233 "2.6M" (cut, not
// rounded, with one decimal below 10).
static void short_count(char* out, size_t len, double count)
{
	const char* unit = "";
	if (count >= 1e9)
	{
		count /= 1e9;
		unit = "B";
	}
	else if (count >= 1e6)
	{
		count /= 1e6;
		unit = "M";
	}
	else if (count >= 1e3)
	{
		count /= 1e3;
		unit = "K";
	}
	const long whole = (long)count;
	const long tenths = (long)(count * 10) % 10;
	if (*unit && count < 10 && tenths) snprintf(out, len, "%ld.%ld%s", whole, tenths, unit);
	else snprintf(out, len, "%ld%s", whole, unit);
}

// Turns "1834567890" into "1.8B views".
static void short_views(char* out, size_t len, const std::string& count)
{
	if (count.empty() || !isdigit((unsigned char)count[0]))
	{
		if (len) out[0] = 0;
		return;
	}
	double v = strtod(count.c_str(), NULL);
	if (v < 1e3)
	{
		snprintf(out, len, "%.0f view%s", v, v == 1 ? "" : "s");
		return;
	}
	char short_form[16];
	short_count(short_form, sizeof(short_form), v);
	snprintf(out, len, "%s views", short_form);
}

// ---------------------------------------------------------------- search
// One query can read several result pages (continuations). Shorts are left out because
// they're vertical videos made for phones and look tiny on the 3DS's wide screen. So are
// upcoming premieres, which have no length and can't be played yet; streams that are live
// right now stay in. A 3D search tries two phrasings, drops what the 3DS can't show in 3D
// (360°, VR180, red-cyan anaglyph) and puts titles that name a side-by-side or top-bottom
// layout first.

struct Found {
	YtItem item;
	bool is_short;
	int score;   // score_3d of the title, for 3D searches
};

// Reads one search result entry into *f. Returns false for entries that aren't a playable video.
static bool parse_video(const Value* r, Found* f)
{
	std::string id = str(get(r, "videoId"));
	if (id.empty() || id.size() >= sizeof(f->item.id)) return false;
	memset(f, 0, sizeof(*f));
	copy_str(f->item.id, sizeof(f->item.id), id);
	std::string title = text_of(get(r, "title"));
	if (title.empty()) title = text_of(get(r, "headline"));
	copy_str(f->item.title, sizeof(f->item.title), title.empty() ? "(untitled)" : title);
	copy_str(f->item.channel, sizeof(f->item.channel), text_of(get(r, "shortBylineText")));
	copy_str(f->item.views, sizeof(f->item.views), text_of(get(r, "shortViewCountText")));
	f->item.duration = parse_duration(text_of(get(r, "lengthText")));
	f->is_short = path(r, { "navigationEndpoint", "reelWatchEndpoint" }) != NULL;
	if (f->item.duration > 0) return true;
	// No length. With a LIVE overlay it's live right now (duration stays 0 and the UI shows
	// "LIVE"). Otherwise it's upcoming, or an ended stream still being processed, and neither
	// can be played.
	if (const Value* ov = get(r, "thumbnailOverlays"))
		if (ov->IsArray())
			for (const Value& o : ov->GetArray())   // NOLINT(clang-analyzer-core.NullDereference): checked IsArray
				if (str(path(&o, { "thumbnailOverlayTimeStatusRenderer", "style" })) == "LIVE") return true;
	return false;
}

static bool has(const std::string& s, const char* word) { return s.find(word) != std::string::npos; }

// Scores how well a title promises 3D that the 3DS can actually show. 3 means it names a
// side-by-side or top-bottom layout, 1 that it only says "3D", and 0 neither. -1 is for 360°,
// VR180 or anaglyph, which would come out flat or in red and cyan here.
static int score_3d(const char* title)
{
	std::string t;
	for (const char* p = title; *p; p++) t += (char)tolower((unsigned char)*p);
	size_t at = t.find("360");
	bool is_360 = at != std::string::npos && (at == 0 || !isdigit((unsigned char)t[at - 1])) &&
	              (at + 3 >= t.size() || (!isdigit((unsigned char)t[at + 3]) && t[at + 3] != 'p')) &&
	              !has(t, "xbox 360");
	if (is_360 || has(t, "vr180") || has(t, "vr 180") || has(t, "180\xC2\xB0") || has(t, "180 vr") ||
	    has(t, "anaglyph") || has(t, "red cyan") || has(t, "red/cyan") || has(t, "red-cyan"))
		return -1;
	// "side by side" on its own usually means a comparison video, so like app_title_mode we
	// only count it together with "3D"
	bool is3d = has(t, "3d");
	if (has(t, "sbs") || has(t, "h-ou") || has(t, "half ou") ||
	    (is3d && (has(t, "side by side") || has(t, "side-by-side") || has(t, "sidebyside") || has(t, "top bottom") ||
	              has(t, "top-bottom") || has(t, "top and bottom") || has(t, "top & bottom") || has(t, "over under") ||
	              has(t, "over-under"))))
		return 3;
	return is3d ? 1 : 0;
}

// Fetches one page of results: the first page for query when cont is empty, otherwise the
// page the continuation token names. Appends the videos to found and puts the next page's
// token in next ("" if there is none). Returns false on error.
static bool search_page(const char* query, const std::string& cont, Found* found, int cap, int& n, std::string& next,
                        char* err, size_t errlen)
{
	rapidjson::StringBuffer sb;
	JsonWriter w(sb);
	w.StartObject();
	write_mweb_context(w);
	if (cont.empty()) kv(w, "query", query);   // the writer escapes quotes, backslashes and control characters
	else kv(w, "continuation", cont.c_str());
	w.EndObject();
	std::string body = sb.GetString();

	Body resp;
	std::string e;
	lock(&search_lock);
	long status = http_on(search_curl, innertube_url("search"), &body, resp, NULL, e);
	unlock(&search_lock);
	if (status != 200)
	{
		snprintf(err, errlen, "%s",
		         status < 0 ? e.c_str() : ("YouTube returned HTTP " + std::to_string(status)).c_str());
		return false;
	}
	Document doc;
	if (!parse_json(doc, &resp.data[0]))
	{
		snprintf(err, errlen, "YouTube sent a reply Tube3D doesn't understand.");
		return false;
	}
	// the first page has its sections here, while continuations bring them in an "append" command
	const Value* sections = path(&doc, { "contents", "sectionListRenderer", "contents" });
	if (!sections)
		if (const Value* cmds = get(&doc, "onResponseReceivedCommands"))
			if (cmds->IsArray())
				for (const Value& c :
				     cmds->GetArray())   // NOLINT(clang-analyzer-core.NullDereference): checked IsArray
					if (const Value* items = path(&c, { "appendContinuationItemsAction", "continuationItems" }))
						sections = items;
	next.clear();
	if (!sections || !sections->IsArray()) return true;
	for (const Value& sec : sections->GetArray())
	{
		if (const Value* tok =
		        path(&sec, { "continuationItemRenderer", "continuationEndpoint", "continuationCommand", "token" }))
			next = str(tok);
		const Value* items = path(&sec, { "itemSectionRenderer", "contents" });
		if (!items || !items->IsArray()) continue;
		for (const Value& item : items->GetArray())
		{
			const Value* v = get(&item, "videoWithContextRenderer");
			if (!v) v = get(&item, "compactVideoRenderer");
			if (v && n < cap && parse_video(v, &found[n])) n++;
		}
	}
	return true;
}

// Where a search can carry on from: each phrasing's next page, plus every video already
// handed out.
struct YtSearchCursor {
	bool three_d;
	std::string query[2];   // the phrasings; the second is empty for a plain search
	std::string token[2];   // each one's next page, or "" when it's done
	std::vector<std::string> seen;
};

// Fetches pages for each phrasing that still has more, up to pages_each per phrasing and 4 in
// all, stopping early once enough non-Shorts have turned up. Each starts at c->token[i]; with
// first set, an empty token means the query's first page. Afterwards c->token holds each
// phrasing's next page. Returns false only if no page could be read at all.
static bool fetch_pages(YtSearchCursor* c, bool first, int pages_each, int want, Found* found, int cap, int& n,
                        char* err, size_t errlen)
{
	int pages = 0;
	bool ok = false;
	for (int qi = 0; qi < 2; qi++)
	{
		const std::string& q = c->query[qi];
		if (q.empty() || (!first && c->token[qi].empty())) continue;
		for (int page = 0; page < pages_each && pages < 4; page++, pages++)
		{
			std::string next;
			if (!search_page(q.c_str(), c->token[qi], found, cap, n, next, err, errlen)) break;
			ok = true;
			c->token[qi] = next;
			int usable = 0;
			for (int i = 0; i < n; i++) usable += !found[i].is_short;
			if (next.empty() || usable >= want) break;
		}
	}
	return ok;
}

// Copies the found videos to out and returns how many. Duplicates are skipped, both because
// the two 3D phrasings overlap and so nothing the cursor handed out before comes back. A 3D
// search also drops what can't be shown in 3D and puts the best 3D scores first. Shorts only
// get in when a first page has nothing else.
static int pick_results(YtSearchCursor* c, bool first, Found* found, int n, YtItem* out, int max)
{
	int kept = 0;
	for (int i = 0; i < n; i++)
	{
		bool dup = false;
		for (int j = 0; j < kept && !dup; j++) dup = !strcmp(found[j].item.id, found[i].item.id);
		for (size_t j = 0; j < c->seen.size() && !dup; j++) dup = c->seen[j] == found[i].item.id;
		found[i].score = c->three_d ? score_3d(found[i].item.title) : 0;
		if (!dup && found[i].score >= 0) found[kept++] = found[i];
	}
	int videos = 0;
	for (int i = 0; i < kept; i++) videos += !found[i].is_short;
	int count = 0;
	for (int pass = 3; pass >= 0 && count < max;
	     pass--)   // best 3D score first, keeping YouTube's order within each score
		for (int i = 0; i < kept && count < max; i++)
			if ((c->three_d ? found[i].score == pass : pass == 0) && (!found[i].is_short || (first && !videos)))
			{
				out[count++] = found[i].item;
				c->seen.push_back(found[i].item.id);
			}
	return count;
}

extern "C" int yt_search_paged(const char* query, bool three_d, YtItem* out, int max, YtSearchCursor** more, char* err,
                               size_t errlen)
{
	if (more) *more = NULL;
	if (!curl)
	{
		snprintf(err, errlen, "Networking isn't ready.");
		return -1;
	}
	const int cap = 160;   // entries collected before filtering: two queries, a few pages each
	Found* found = (Found*)calloc(cap, sizeof(Found));
	YtSearchCursor* c = new (std::nothrow) YtSearchCursor();
	if (!found || !c)
	{
		free(found);
		delete c;
		snprintf(err, errlen, "Not enough memory to search.");
		return -1;
	}

	// A 3D search that doesn't mention "3d" tries two common ways uploaders name the layout.
	std::string lower;
	for (const char* p = query; *p; p++) lower += (char)tolower((unsigned char)*p);
	c->three_d = three_d;
	c->query[0] = query;
	if (three_d && !has(lower, "3d"))
	{
		c->query[0] += " 3D SBS";
		c->query[1] = std::string(query) + " 3D side by side";
	}
	else if (three_d) c->query[1] = std::string(query) + " SBS";

	// Keep reading pages until the list is full without Shorts, which are often half a page.
	// That's up to 3 pages for a plain search and 2 per phrasing for a 3D one, since each page
	// takes about a second on the 3DS.
	int n = 0;
	if (!fetch_pages(c, true, c->query[1].empty() ? 3 : 2, max * (three_d ? 2 : 1), found, cap, n, err, errlen))
	{
		free(found);
		delete c;
		return -1;
	}
	int count = pick_results(c, true, found, n, out, max);
	free(found);
	if (more && (!c->token[0].empty() || !c->token[1].empty())) *more = c;
	else delete c;
	return count;
}

extern "C" int yt_search_more(YtSearchCursor* c, YtItem* out, int max, char* err, size_t errlen)
{
	if (!c) return 0;
	if (!curl)
	{
		snprintf(err, errlen, "Networking isn't ready.");
		return -1;
	}
	const int cap = 120;
	Found* found = (Found*)calloc(cap, sizeof(Found));
	if (!found)
	{
		snprintf(err, errlen, "Not enough memory to search.");
		return -1;
	}
	// Keep going until something new turns up; a page can be all Shorts or videos we've shown.
	int count = 0;
	for (int round = 0; round < 3 && count == 0 && (!c->token[0].empty() || !c->token[1].empty()); round++)
	{
		int n = 0;
		if (!fetch_pages(c, false, 1, max * (c->three_d ? 2 : 1), found, cap, n, err, errlen))
		{
			free(found);
			return -1;
		}
		count = pick_results(c, false, found, n, out, max);
	}
	free(found);
	return count;
}

extern "C" void yt_search_free(YtSearchCursor* c) { delete c; }

// ---------------------------------------------------------------- comments
// The MWEB watch page (the "next" call) has a comments section, identified by
// sectionIdentifier "comment-item-section". Its continuation token fetches the first page of
// comments in the older commentThreadRenderer/commentRenderer format, which is about a third
// the size of what the WEB client gets. Each page ends with the next page's token. None of
// this needs a login, a PO token or visitorData.

struct YtCommentsCursor {
	std::string token;
};

// Finds the first value named key anywhere under v (depth-first), or returns NULL.
static const Value* find_key(const Value* v, const char* key, int depth = 0)
{
	if (!v || depth > 40) return NULL;
	if (v->IsObject())
	{
		if (const Value* hit = get(v, key)) return hit;
		for (Value::ConstMemberIterator it = v->MemberBegin(); it != v->MemberEnd(); ++it)
			if (const Value* hit = find_key(&it->value, key, depth + 1)) return hit;
	}
	else if (v->IsArray())
		for (const Value& e : v->GetArray())   // NOLINT(clang-analyzer-core.NullDereference): checked IsArray
			if (const Value* hit = find_key(&e, key, depth + 1)) return hit;
	return NULL;
}

// Finds the itemSectionRenderer whose sectionIdentifier is "comment-item-section", or NULL.
static const Value* find_comment_section(const Value* v, int depth = 0)
{
	if (!v || depth > 40) return NULL;
	if (v->IsObject())
	{
		if (const Value* s = get(v, "itemSectionRenderer"))
			if (str(get(s, "sectionIdentifier")) == "comment-item-section") return s;
		for (Value::ConstMemberIterator it = v->MemberBegin(); it != v->MemberEnd(); ++it)
			if (const Value* hit = find_comment_section(&it->value, depth + 1)) return hit;
	}
	else if (v->IsArray())
		for (const Value& e : v->GetArray())   // NOLINT(clang-analyzer-core.NullDereference): checked IsArray
			if (const Value* hit = find_comment_section(&e, depth + 1)) return hit;
	return NULL;
}

// Makes an InnerTube "next" call as MWEB, with either a videoId or a continuation token, and
// parses the reply into doc.
static bool next_request(const char* field, const std::string& value, Body& resp, Document& doc, char* err,
                         size_t errlen)
{
	rapidjson::StringBuffer sb;
	JsonWriter w(sb);
	w.StartObject();
	write_mweb_context(w);
	kv(w, field, value.c_str());
	w.EndObject();
	std::string body = sb.GetString();
	std::string e;
	lock(&comments_lock);
	long status =
	    comments_cancel ? -1 : http_on(comments_curl, innertube_url("next"), &body, resp, NULL, e, &comments_cancel);
	unlock(&comments_lock);
	if (status != 200)
	{
		snprintf(err, errlen, "%s",
		         comments_cancel ? "Cancelled."
		         : status < 0    ? e.c_str()
		                         : ("YouTube returned HTTP " + std::to_string(status)).c_str());
		return false;
	}
	if (!parse_json(doc, &resp.data[0]))
	{
		snprintf(err, errlen, "YouTube sent a reply Tube3D doesn't understand.");
		return false;
	}
	return true;
}

// Copies comment text, keeping newlines (they separate paragraphs) but dropping other
// control characters. Text that's too long is cut at a character boundary.
static void copy_comment_text(char* dst, size_t cap, const std::string& s)
{
	size_t n = 0;
	for (size_t i = 0; i < s.size() && n + 1 < cap; i++)
	{
		unsigned char c = (unsigned char)s[i];
		if (c < 0x20 && c != '\n') continue;
		dst[n++] = (char)c;
	}
	if (n + 1 >= cap) n = utf8_whole_len(dst, n);   // cut short
	dst[n] = 0;
}

// Fetches the page of comments c->token points at into out. Afterwards c->token is the next
// page's token, or "" if there isn't one.
static int comments_page(YtCommentsCursor* c, YtComment* out, int max, char* err, size_t errlen)
{
	Body resp;
	Document doc;
	if (!next_request("continuation", c->token, resp, doc, err, errlen)) return -1;
	c->token.clear();
	int n = 0;
	const Value* endpoints = get(&doc, "onResponseReceivedEndpoints");
	if (!endpoints || !endpoints->IsArray()) return 0;
	for (const Value& ep : endpoints->GetArray())   // NOLINT(clang-analyzer-core.NullDereference): checked IsArray
	{
		const Value* items = path(&ep, { "reloadContinuationItemsCommand", "continuationItems" });
		if (!items) items = path(&ep, { "appendContinuationItemsAction", "continuationItems" });
		if (!items || !items->IsArray()) continue;
		for (const Value& entry : items->GetArray())   // NOLINT(clang-analyzer-core.NullDereference): checked IsArray
		{
			if (const Value* cont = get(&entry, "continuationItemRenderer"))
			{
				if (const Value* tok = find_key(cont, "continuationCommand")) c->token = str(get(tok, "token"));
				continue;
			}
			const Value* renderer = path(&entry, { "commentThreadRenderer", "comment", "commentRenderer" });
			if (!renderer || n >= max) continue;
			YtComment* comment = &out[n++];
			memset(comment, 0, sizeof(*comment));
			copy_str(comment->author, sizeof(comment->author), text_of(get(renderer, "authorText")));
			copy_str(comment->when, sizeof(comment->when), text_of(get(renderer, "publishedTimeText")));
			copy_str(comment->likes, sizeof(comment->likes), text_of(get(renderer, "voteCount")));
			comment->replies = num(get(renderer, "replyCount"));
			const Value* owner = get(renderer, "authorIsChannelOwner");
			comment->by_uploader = owner && owner->IsBool() && owner->GetBool();
			comment->pinned = get(renderer, "pinnedCommentBadge") != NULL;
			copy_comment_text(comment->text, sizeof(comment->text), text_of(get(renderer, "contentText")));
		}
	}
	return n;
}

extern "C" int yt_comments(const char* video_id, YtComment* out, int max, YtCommentsCursor** more, char* err,
                           size_t errlen)
{
	*more = NULL;
	if (!curl)
	{
		snprintf(err, errlen, "Networking isn't ready.");
		return -1;
	}
	YtCommentsCursor* c = new (std::nothrow) YtCommentsCursor();
	if (!c)
	{
		snprintf(err, errlen, "Not enough memory.");
		return -1;
	}
	{
		// all we need from the watch page is the comments section's token
		Body resp;
		Document doc;
		if (!next_request("videoId", video_id, resp, doc, err, errlen))
		{
			delete c;
			return -1;
		}
		const Value* sec = find_comment_section(&doc);
		const Value* tok = sec ? find_key(sec, "continuationCommand") : NULL;
		c->token = tok ? str(get(tok, "token")) : "";
	}
	if (c->token.empty())
	{
		delete c;
		snprintf(err, errlen, "Comments are turned off.");
		return -1;
	}
	int n = comments_page(c, out, max, err, errlen);
	if (n < 0 || c->token.empty()) delete c;
	else *more = c;
	return n;
}

extern "C" int yt_comments_more(YtCommentsCursor* c, YtComment* out, int max, char* err, size_t errlen)
{
	if (!c || c->token.empty()) return 0;
	if (!curl)
	{
		snprintf(err, errlen, "Networking isn't ready.");
		return -1;
	}
	return comments_page(c, out, max, err, errlen);
}

extern "C" void yt_comments_free(YtCommentsCursor* c) { delete c; }

// ---------------------------------------------------------------- likes and dislikes
// YouTube still shows how many people liked a video, but it stopped publishing dislikes in
// 2021. For those we ask the Return YouTube Dislike project, which estimates them from its own
// users' votes. It knows the like count too, which we fall back on if YouTube's is missing.

#define DISLIKE_API "https://returnyoutubedislikeapi.com/votes?videoId="

// Reads the label on the like button of the mobile watch page, which is already in short form.
static void youtube_likes(const char* video_id, char* likes, size_t likes_len)
{
	Body resp;
	Document doc;
	char err[96];
	if (!next_request("videoId", video_id, resp, doc, err, sizeof(err))) return;
	const Value* button = find_key(&doc, "likeButtonViewModel");
	if (const Value* inner = get(button, "likeButtonViewModel")) button = inner;   // YouTube nests it twice
	const Value* toggle = get(get(button, "toggleButtonViewModel"), "toggleButtonViewModel");
	const std::string label = str(get(get(get(toggle, "defaultButtonViewModel"), "buttonViewModel"), "title"));
	if (!label.empty() && isdigit((unsigned char)label[0])) copy_str(likes, likes_len, label);
}

// Asks Return YouTube Dislike for its counts. The likes are only filled in if still empty.
static void estimated_votes(const char* video_id, char* likes, size_t likes_len, char* dislikes, size_t dislikes_len)
{
	Body resp;
	std::string err;
	lock(&comments_lock);
	const long status = comments_cancel ? -1
	                                    : http_on(comments_curl, std::string(DISLIKE_API) + video_id, NULL, resp, NULL,
	                                              err, &comments_cancel);
	unlock(&comments_lock);
	Document doc;
	if (status != 200 || !parse_json(doc, &resp.data[0])) return;
	const Value* down = get(&doc, "dislikes");
	const Value* up = get(&doc, "likes");
	if (down && down->IsNumber()) short_count(dislikes, dislikes_len, down->GetDouble());
	if (!likes[0] && up && up->IsNumber()) short_count(likes, likes_len, up->GetDouble());
}

extern "C" bool yt_votes(const char* video_id, char* likes, size_t likes_len, char* dislikes, size_t dislikes_len)
{
	if (likes_len) likes[0] = 0;
	if (dislikes_len) dislikes[0] = 0;
	if (!curl) return false;
	youtube_likes(video_id, likes, likes_len);
	if (!comments_cancel) estimated_votes(video_id, likes, likes_len, dislikes, dislikes_len);
	return likes[0] || dislikes[0];
}

// visitorData makes the ANDROID player client return streams more reliably. The caller holds
// curl_lock. Only yt_get_streams uses this, so the request can be cancelled along with it.
static const std::string& get_visitor_data()
{
	if (!visitor_data.empty()) return visitor_data;
	Body resp;
	std::string e;
	if (http("https://www.youtube.com/sw.js_data", NULL, resp, "Referer: https://www.youtube.com/", e,
	         &streams_cancel) != 200)
		return visitor_data;
	std::string& reply = resp.data;
	size_t start = reply.find('[');   // skip the ")]}'" anti-JSON-hijacking prefix
	if (start == std::string::npos) return visitor_data;
	Document doc;
	if (!parse_json(doc, &reply[start]) || !doc.IsArray()) return visitor_data;
	// it lives at [0][2][0][0][13], the same path FourthTube uses
	const Value* v = &doc;
	const int idx[] = { 0, 2, 0, 0, 13 };
	for (int i : idx)
	{
		if (!v->IsArray() || (int)v->Size() <= i) return visitor_data;
		v = &(*v)[i];
	}
	visitor_data = str(v);
	return visitor_data;
}

// One stream from streamingData, with what we need to check it before playing.
struct Fmt {
	std::string url;
	int width = 0, height = 0;
	uint64_t length = 0;           // contentLength, 0 if missing
	uint64_t duration_ms = 0;      // approxDurationMs
	uint64_t bitrate = 0;          // averageBitrate, else bitrate (bits/s)
	int stereo = YT_STEREO_NONE;   // from stereoLayout, which only the VISIONOS client gets

	// A file offset near the end to probe, or 0 if the size is unknown. Without a
	// contentLength we estimate the size from duration times bitrate, and probe at 80% of
	// that so an estimate on the high side doesn't ask for a byte past the end.
	uint64_t probe_offset() const
	{
		if (length > 1) return length * 9 / 10;
		uint64_t est = duration_ms * bitrate / 8000;
		return est * 8 / 10;
	}
};

static Fmt format_of(const Value* f, const std::string& url, int width, int height)
{
	Fmt fmt;
	fmt.url = url_decode(url);
	fmt.width = width;
	fmt.height = height;
	fmt.length = strtoull(str(get(f, "contentLength")).c_str(), NULL, 10);
	fmt.duration_ms = strtoull(str(get(f, "approxDurationMs")).c_str(), NULL, 10);
	int bitrate = num(get(f, "averageBitrate"));
	if (bitrate <= 0) bitrate = num(get(f, "bitrate"));
	fmt.bitrate = bitrate > 0 ? (uint64_t)bitrate : 0;
	std::string layout = str(get(f, "stereoLayout"));   // STEREO_LAYOUT_LEFT_RIGHT, ..._TOP_BOTTOM
	fmt.stereo = layout.find("LEFT_RIGHT") != std::string::npos   ? YT_STEREO_SBS
	             : layout.find("TOP_BOTTOM") != std::string::npos ? YT_STEREO_TB
	                                                              : YT_STEREO_NONE;
	return fmt;
}

// Which InnerTube identity asks for the streams. Without a PO token, YouTube gives the plain
// ANDROID client only the first minute or so of the separate (DASH) streams and answers 403
// after that, which would leave the muxed 360p stream as the only full-length choice. Two
// identities do get full-length DASH without a PO token, a login or YouTube's player
// JavaScript. Both were found by others (the Kodi YouTube add-on and yt-dlp):
//
//   P_ANDROID   ANDROID with params "2AMB", a test-suite flag. Also offers the muxed stream.
//   P_VISIONOS  the Apple Vision Pro client. Needs visitorData and has no muxed stream.
//
// These are the kind of loophole YouTube tends to close, so every DASH pick is still checked
// with an end-of-file probe, and if both fail we fall back to the muxed 360p stream.
enum PlayerClient { P_ANDROID, P_VISIONOS };

// Sends the player request. On success doc holds the reply, parsed in place inside resp.
static bool player_request(const char* video_id, PlayerClient client, Body& resp, Document& doc, char* err,
                           size_t errlen)
{
	lock(&curl_lock);
	std::string visitor = get_visitor_data();

	rapidjson::StringBuffer sb;
	JsonWriter w(sb);
	w.StartObject();
	kv(w, "videoId", video_id);
	w.Key("context");
	w.StartObject();
	w.Key("client");
	w.StartObject();
	kv(w, "hl", "en");
	kv(w, "gl", "US");
	if (client == P_VISIONOS)
	{
		kv(w, "clientName", "VISIONOS");
		kv(w, "clientVersion", "1.02");
		kv(w, "deviceMake", "Apple");
		kv(w, "deviceModel", "RealityDevice17,1");
		kv(w, "osName", "visionOS");
		kv(w, "osVersion", "26.5.23O471");
	}
	else
	{
		kv(w, "clientName", "ANDROID");
		kv(w, "clientVersion", "20.10.38");
		kv(w, "deviceMake", "Apple");
		kv(w, "deviceModel", "iPhone9,1");
		kv(w, "osName", "iPhone");
		kv(w, "userAgent", "com.google.ios.youtube/19.01.1 (iPhone9,1; U; CPU iOS 15_0_0 like Mac OS X;)");
		kv(w, "osVersion", "15.0.0.19A346");
	}
	if (!visitor.empty()) kv(w, "visitorData", visitor.c_str());
	w.EndObject();
	w.EndObject();
	if (client == P_ANDROID)
	{
		w.Key("playbackContext");
		w.StartObject();
		w.Key("contentPlaybackContext");
		w.StartObject();
		kv(w, "signatureTimestamp", "0");
		w.EndObject();
		w.EndObject();
		kv(w, "params", "2AMB");
	}
	w.Key("contentCheckOk");
	w.Bool(true);
	w.Key("racyCheckOk");
	w.Bool(true);
	w.EndObject();
	std::string body = sb.GetString();

	std::string e;
	long status = streams_cancel ? -1 : http(innertube_url("player"), &body, resp, NULL, e, &streams_cancel);
	unlock(&curl_lock);
	if (streams_cancel)
	{
		snprintf(err, errlen, "Cancelled");
		return false;
	}
	if (status != 200)
	{
		snprintf(err, errlen, "%s",
		         status < 0 ? e.c_str() : ("YouTube returned HTTP " + std::to_string(status)).c_str());
		return false;
	}
	if (!parse_json(doc, &resp.data[0]))
	{
		snprintf(err, errlen, "YouTube sent a reply Tube3D doesn't understand.");
		return false;
	}
	return true;
}

// Picks the DASH H.264 video (the tallest within the size limits) and AAC audio (itag 140 if
// there is one) from a player reply. When info is given it also fills in the 360 and live
// flags and a missing duration; muxed, if given, receives the muxed stream.
static void scan_formats(const Document& doc, int max_height, Fmt& video, Fmt& audio, std::string* muxed,
                         YtStreams* info)
{
	bool have_140 = false;
	const char* lists[] = { "formats", "adaptiveFormats" };
	for (const char* list : lists)
	{
		const Value* formats = path(&doc, { "streamingData", list });
		if (!formats || !formats->IsArray()) continue;
		for (const Value& f : formats->GetArray())
		{
			if (info && get(&f, "targetDurationSec")) info->is_live = true;   // a live, segmented format
			std::string url = str(get(&f, "url"));
			if (url.empty()) continue;   // cipher-protected, which we can't play without base.js
			if (str(get(&f, "type")) == "FORMAT_STREAM_TYPE_OTF") continue;
			int itag = num(get(&f, "itag"));
			int height = num(get(&f, "height"));
			int width = num(get(&f, "width"));
			if (width <= 0) width = (height * 16 + 8) / 9;   // not given, so assume 16:9
			std::string mime = str(get(&f, "mimeType"));
			if (info)
			{
				std::string proj = str(get(&f, "projectionType"));
				if (proj == "EQUIRECTANGULAR" || proj == "MESH") info->is_360 = true;
				if (!info->duration_ms)
					info->duration_ms = (uint32_t)strtoul(str(get(&f, "approxDurationMs")).c_str(), NULL, 10);
			}

			if (itag == 18)
			{
				if (muxed) *muxed = url_decode(url);
				continue;
			}
			if (mime.compare(0, 5, "video") == 0 && mime.find("avc1") != std::string::npos)
			{
				// The player's video texture is at most 1024 px wide, so the taller tiers of a
				// full-SBS or 21:9 upload don't fit even when their height does.
				if (height <= max_height && width <= YT_MAX_VIDEO_WIDTH && height > video.height)
					video = format_of(&f, url, width, height);
			}
			else if (mime.compare(0, 5, "audio") == 0 && mime.find("mp4a") != std::string::npos)
			{
				if (itag == 140)
				{
					audio = format_of(&f, url, 0, 0);
					have_140 = true;
				}
				else if (!have_140 && audio.url.empty()) audio = format_of(&f, url, 0, 0);
			}
		}
	}
	if (video.url.empty() || audio.url.empty()) video = audio = Fmt();   // DASH needs both
}

// True when YouTube serves a byte near the end of both DASH streams, meaning they aren't cut
// off after the first minute. A stream of unknown length can't be checked and counts as cut off.
static bool dash_full_length(const Fmt& video, const Fmt& audio)
{
	uint64_t video_at = video.probe_offset(), audio_at = audio.probe_offset();
	return video_at && audio_at && range_ok(video.url, video_at) && range_ok(audio.url, audio_at);
}

// ---- live streams
// The ANDROID client's hlsManifestUrl is a master playlist of muxed variants: H.264 and AAC
// together in 5 s MPEG-TS chunks (itag 93 is 360p, 94 is 480p, and so on). No PO token is
// needed. We take the tallest H.264 variant within max_height and YT_MAX_VIDEO_WIDTH, and
// livesrc.c follows its media playlist. muxed_url looks like
// "live:w=854,h=480,fps=30000,lv=31;<media playlist>", carrying the size, frame rate and
// H.264 level from the master playlist so the source doesn't have to parse the stream's SPS.
static bool live_streams(const Document& doc, int max_height, YtStreams* out)
{
	out->duration_ms = 0;
	std::string master = str(path(&doc, { "streamingData", "hlsManifestUrl" }));
	if (master.empty())
	{
		snprintf(out->error, sizeof(out->error), "This live stream can't be played here (no HLS stream).");
		return false;
	}
	Body resp;
	std::string e;
	lock(&curl_lock);
	long status = streams_cancel ? -1 : http(master, NULL, resp, NULL, e, &streams_cancel);
	unlock(&curl_lock);
	if (status != 200)
	{
		snprintf(out->error, sizeof(out->error), "The live stream's playlist couldn't be loaded (%s).",
		         status < 0 ? e.c_str() : ("HTTP " + std::to_string(status)).c_str());
		return false;
	}
	// Each variant is a line like
	// #EXT-X-STREAM-INF:BANDWIDTH=...,CODECS="avc1.4D401F,mp4a.40.2",RESOLUTION=854x480,FRAME-RATE=30
	// followed by the variant's URL.
	std::string best;
	int best_w = 0, best_h = 0, best_fps1000 = 30000, best_level = 31;
	const std::string& text = resp.data;
	size_t pos = 0;
	std::string stream_inf;
	while (pos < text.size())
	{
		size_t nl = text.find('\n', pos);
		if (nl == std::string::npos) nl = text.size();
		std::string line = text.substr(pos, nl - pos);
		pos = nl + 1;
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.compare(0, 18, "#EXT-X-STREAM-INF:") == 0)
		{
			stream_inf = line;
			continue;
		}
		if (line.empty() || line[0] == '#' || stream_inf.empty()) continue;
		size_t codecs_at = stream_inf.find("CODECS=\"");
		std::string codecs =
		    codecs_at == std::string::npos
		        ? ""
		        : stream_inf.substr(codecs_at + 8, stream_inf.find('"', codecs_at + 8) - codecs_at - 8);
		size_t res_at = stream_inf.find("RESOLUTION=");
		int w = res_at == std::string::npos ? 0 : (int)strtol(stream_inf.c_str() + res_at + 11, NULL, 10);
		size_t x_at = res_at == std::string::npos ? std::string::npos : stream_inf.find('x', res_at);
		int h = x_at == std::string::npos ? 0 : (int)strtol(stream_inf.c_str() + x_at + 1, NULL, 10);
		double fps = 0;
		size_t fps_at = stream_inf.find("FRAME-RATE=");
		if (fps_at != std::string::npos) fps = strtod(stream_inf.c_str() + fps_at + 11, NULL);
		stream_inf.clear();
		if (codecs.find("avc1.") == std::string::npos || codecs.find("mp4a.") == std::string::npos)
			continue;   // needs H.264 and sound
		if (h > max_height || w > YT_MAX_VIDEO_WIDTH || h <= best_h) continue;
		best = line;
		best_w = w;
		best_h = h;
		best_fps1000 = fps > 0 ? (int)lround(fps * 1000) : 30000;
		size_t avc_at = codecs.find("avc1.");
		// In avc1.PPCCLL, LL is the level times 10. If the CODECS value is too short, assume
		// the usual 3.1.
		best_level = avc_at + 11 <= codecs.size() ? (int)strtol(codecs.substr(avc_at + 9, 2).c_str(), NULL, 16) : 31;
		if (best_level <= 0) best_level = 31;
	}
	if (best.empty())
	{
		snprintf(out->error, sizeof(out->error), "This live stream has no picture size the 3DS can show.");
		return false;
	}
	char head[64];
	snprintf(head, sizeof(head), "live:w=%d,h=%d,fps=%d,lv=%d;", best_w, best_h, best_fps1000, best_level);
	out->muxed_url = strdup((head + best).c_str());
	out->video_height = best_h;
	out->video_width = best_w;
	if (!out->muxed_url)
	{
		snprintf(out->error, sizeof(out->error), "Not enough memory.");
		return false;
	}
	return true;
}

extern "C" bool yt_get_streams(const char* video_id, int max_height, YtStreams* out)
{
	memset(out, 0, sizeof(*out));
	streams_cancel = false;   // a cancel only ever ends the lookup it was meant for
	if (!curl)
	{
		snprintf(out->error, sizeof(out->error), "Networking isn't ready.");
		return false;
	}

	Body resp;
	Document doc;
	if (!player_request(video_id, P_ANDROID, resp, doc, out->error, sizeof(out->error))) return false;

	const Value* details = get(&doc, "videoDetails");
	copy_str(out->title, sizeof(out->title), str(get(details, "title")));
	copy_str(out->author, sizeof(out->author), str(get(details, "author")));
	short_views(out->views, sizeof(out->views), str(get(details, "viewCount")));
	out->duration_ms = (uint32_t)num(get(details, "lengthSeconds")) * 1000u;
	const Value* is_live = get(details, "isLive");
	out->is_live = is_live && is_live->IsBool() && is_live->GetBool();

	bool length_known = out->duration_ms > 0;

	std::string playable = str(path(&doc, { "playabilityStatus", "status" }));
	if (playable != "OK")
	{
		std::string reason = str(path(&doc, { "playabilityStatus", "reason" }));
		snprintf(
		    out->error, sizeof(out->error), "%s",
		    reason.empty()
		        ? ("YouTube says the video is " + (playable.empty() ? std::string("unavailable") : playable)).c_str()
		        : reason.c_str());
		return false;
	}

	// A live stream plays from HLS, MPEG-TS chunks with picture and sound together.
	if (out->is_live) return live_streams(doc, max_height, out);

	// Subtitle tracks: the uploader's own first, then speech recognition ("asr").
	const Value* tracks = path(&doc, { "captions", "playerCaptionsTracklistRenderer", "captionTracks" });
	for (int pass = 0; pass < 2 && tracks && tracks->IsArray(); pass++)
		for (const Value& track : tracks->GetArray())
		{
			bool asr = str(get(&track, "kind")) == "asr";
			std::string url = str(get(&track, "baseUrl"));
			if (asr != (pass == 1) || url.compare(0, 8, "https://") != 0 || out->caption_count >= YT_MAX_CAPTIONS)
				continue;
			url = with_param(url, "fmt", "srv3");   // timed paragraphs for subs.c; usually it's there already
			YtCaption* caption = &out->captions[out->caption_count];
			copy_str(caption->lang, sizeof(caption->lang), str(get(&track, "languageCode")));
			std::string name = text_of(get(&track, "name"));
			copy_str(caption->name, sizeof(caption->name), name.empty() ? caption->lang : name);
			caption->auto_generated = asr;
			caption->url = strdup(url.c_str());
			if (caption->url) out->caption_count++;
		}

	std::string muxed;
	Fmt video, audio;
	scan_formats(doc, max_height, video, audio, &muxed, out);
	if (!out->is_live && !length_known && !str(path(&doc, { "streamingData", "hlsManifestUrl" })).empty())
		out->is_live = true;   // an HLS manifest and no length means it's live
	if (out->is_live)
	{
		for (int i = 0; i < out->caption_count; i++) free(out->captions[i].url);
		out->caption_count = 0;
		return live_streams(doc, max_height, out);
	}

	// Use the separate streams only if they play to the end (see PlayerClient): VISIONOS's
	// first, then ANDROID+2AMB's, otherwise the muxed 360p stream. With no muxed stream to fall
	// back on, unchecked DASH beats nothing.
	// VISIONOS goes first because it's the only client YouTube tells about 3D: for a marked 3D
	// upload it lists the frame-packed streams with their stereoLayout, while ANDROID gets a
	// flat one-eye version that plays in 2D whatever layout is chosen. ANDROID's reply still
	// supplies the captions and the muxed stream.
	bool vis_used = false;
	if (!streams_cancel)
	{
		Body vis_resp;
		Document vis_doc;
		char vis_err[200];
		Fmt vis_video, vis_audio;
		if (player_request(video_id, P_VISIONOS, vis_resp, vis_doc, vis_err, sizeof(vis_err)) &&
		    str(path(&vis_doc, { "playabilityStatus", "status" })) == "OK")
		{
			scan_formats(vis_doc, max_height, vis_video, vis_audio, NULL, NULL);
			if (!vis_video.url.empty() && (muxed.empty() || dash_full_length(vis_video, vis_audio)))
			{
				video = vis_video;
				audio = vis_audio;
				vis_used = true;
			}
		}
	}
	if (!vis_used && !video.url.empty() && !muxed.empty() && !dash_full_length(video, audio))
	{
		video = audio = Fmt();
		out->dash_cut_off = true;
	}
	if (streams_cancel)   // this also keeps a probe aborted by the cancel from counting as a refusal
	{
		snprintf(out->error, sizeof(out->error), "Cancelled");
		return false;
	}

	if (!muxed.empty() || !video.url.empty())
	{
		out->video_url = dup_or_null(video.url);
		out->audio_url = dup_or_null(audio.url);
		out->muxed_url = dup_or_null(muxed);
		out->video_height = video.url.empty() ? 0 : video.height;
		out->video_width = video.url.empty() ? 0 : video.width;
		out->stereo = video.url.empty() ? YT_STEREO_NONE : video.stereo;
		return true;
	}
	snprintf(out->error, sizeof(out->error), "YouTube didn't offer a stream the 3DS can play (H.264 MP4).");
	return false;
}

struct FixedBuf {
	unsigned char* buf;
	size_t cap, len;
	bool overflow;
};

static size_t on_fixed(char* data, size_t size, size_t n, void* user)
{
	FixedBuf* fixed = (FixedBuf*)user;
	size_t len = size * n;
	if (fixed->len + len > fixed->cap)
	{
		fixed->overflow = true;
		return 0;
	}
	memcpy(fixed->buf + fixed->len, data, len);
	fixed->len += len;
	return len;
}

extern "C" void* yt_http_new(void) { return curl_easy_init(); }
extern "C" void yt_http_free(void* h)
{
	if (h) curl_easy_cleanup((CURL*)h);
}

extern "C" int yt_http_get(void* handle, const char* url, unsigned char* buf, size_t cap, size_t* len)
{
	return yt_http_get_ex(handle, url, buf, cap, len, NULL);
}

extern "C" int yt_http_get_ex(void* handle, const char* url, unsigned char* buf, size_t cap, size_t* len,
                              volatile bool* cancel)
{
	CURL* c = (CURL*)handle;
	FixedBuf fixed = { buf, cap, 0, false };
	*len = 0;
	if (!c) return -1;
	curl_easy_reset(c);
	yt_setup_curl(c);
	curl_easy_setopt(c, CURLOPT_XFERINFODATA, (void*)cancel);
	curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");   // gzip if offered; captions shrink about 5x
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_fixed);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &fixed);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
	CURLcode rc = curl_easy_perform(c);
	if (rc != CURLE_OK || fixed.overflow) return -1;
	long status = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
	*len = fixed.len;
	return (int)status;
}

extern "C" void yt_free_streams(YtStreams* s)
{
	free(s->video_url);
	free(s->audio_url);
	free(s->muxed_url);
	s->video_url = s->audio_url = s->muxed_url = NULL;
	for (int i = 0; i < s->caption_count; i++)
	{
		free(s->captions[i].url);
		s->captions[i].url = NULL;
	}
	s->caption_count = 0;
}
