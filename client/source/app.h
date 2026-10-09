#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <string.h>
#include "playmode.h"

const char* app_build_id(void);   // "1.0.0 (e206264)": the version and commit (buildinfo.c, recompiled on every build)

// Copies text that may need shortening (titles, messages, settings) into a fixed buffer.
// Unlike snprintf("%s"), truncating is expected here, so there are no warnings, and it never
// cuts a UTF-8 character in half (that would show as a broken glyph after a long title).
static inline void str_copy(char* dst, size_t size, const char* src)
{
	size_t n = 0;
	while (n + 1 < size && src[n])
	{
		dst[n] = src[n];
		n++;
	}
	// If we stopped in the middle of a multi-byte character, drop that character.
	if (n + 1 == size)
		while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
	dst[n] = 0;
}

#define MAX_RESULTS  200   // search results grow as you browse, a page at a time near the end
#define SEARCH_FIRST 30    // how many videos a new search lists at first
#define MAX_HISTORY  20
#define MAX_FILES    200

typedef struct {
	char id[24];
	int duration;   // seconds; 0 for live or unknown
	char channel[64];
	char title[256];       // UTF-8
	char views[24];        // YouTube's short view count, e.g. "1.2M views"; "" if unknown
	char path[256];        // a file in the Videos tab (id is empty then), otherwise ""
	bool folder;           // a folder in the Videos tab; path is where it leads
	bool other;            // a file that isn't a video: listed, but not playable
	uint32_t resume_ms;    // for a file left part-way (resume.h), where it stopped and how
	uint32_t resume_dur;   // long it is; resume_ms is 0 if there's nothing to resume
} VideoEntry;

typedef enum { TAB_HOME, TAB_PICKS, TAB_HISTORY, TAB_FILES, TAB_SETTINGS, TAB_COUNT } Tab;

// The experimental "Motion" setting decides how video frames reach the 59.83 Hz screen.
//   NORMAL  Each frame is shown whole and held for 2 or 3 refreshes, so 24 fps video
//           judders in a 2,3,2,3 rhythm.
//   SMOOTH  An in-between picture is drawn at every refresh from the motion between two
//           frames (motion.c). Where frames can't be matched, such as at cuts, it falls
//           back to whole frames.

enum { MOTION_NORMAL, MOTION_SMOOTH, MOTION_COUNT };

typedef struct {
	bool audio_ok;
	PlayMode mode;           // playback mode chosen in Settings
	int menu_depth;          // menu 3D strength: 0 off, 1 low, 2 normal (default), 3 strong
	bool subtitles;          // start videos with subtitles on, in the console's language if offered
	bool nerd_stats;         // show the stats box over the video and memory use on the remote
	int motion;              // MOTION_NORMAL or MOTION_SMOOTH
	bool settings_changed;   // the player's settings page changed one of these: save them

	Tab tab;
	int set_page;   // which Settings page is shown: 0 3D, 1 Playback, 2 More
	VideoEntry results[MAX_RESULTS];
	int nresults;
	VideoEntry picks[MAX_RESULTS];
	int npicks;
	VideoEntry history[MAX_HISTORY];
	int nhistory;
	VideoEntry files[MAX_FILES];
	int nfiles;   // the Videos tab (files.c)
	char files_dir[256];
	int sel[TAB_COUNT];
	float anim[TAB_COUNT];   // animated carousel position

	char query[101];
	char status[160];     // one-line message shown on the bottom screen
	volatile bool busy;   // a search is running
	char busy_text[64];
	bool loading_more;   // the next page of results is loading in the background (no overlay)

	const VideoEntry* now_playing;
	u64 frame;
} App;

extern App app;

// The carousel list for a tab. Settings has none of its own and shows the Home list.
VideoEntry* app_list(Tab t, int* count);

// "sbs", "tb" or "2d", from the mode chosen in Settings or, in Auto, from the video's title.
const char* app_layout(const VideoEntry* e);

// The quick picks shown under the search field when nothing is listed. three_d makes it a 3D search.
typedef struct {
	const char* label;
	const char* query;
	bool three_d;
} QuickPick;
#define QUICK_PICKS 6
extern const QuickPick quick_picks[QUICK_PICKS];

// The 3D layout a title mentions. This is only a first guess, since titles are wrong in both
// directions, so the player also checks the picture itself.
PlayMode app_title_mode(const char* title);
