// Shared app state and helpers used by both main.c and ui.c.
#include "app.h"

#include <ctype.h>
#include <string.h>

App app;

VideoEntry* app_list(Tab t, int* count)
{
	switch (t)
	{
	case TAB_PICKS:   *count = app.npicks; return app.picks;
	case TAB_HISTORY: *count = app.nhistory; return app.history;
	case TAB_FILES:   *count = app.nfiles; return app.files;
	default:          *count = app.nresults; return app.results;
	}
}

const char* play_mode_name(PlayMode m)
{
	static const char* names[MODE_COUNT] = {
		"Auto", "2D", "3D side-by-side", "3D top-bottom", "3D SBS (eyes swapped)", "3D TB (eyes swapped)"
	};
	return m < MODE_COUNT ? names[m] : "?";
}

static bool has_word(const char* hay, const char* word)
{
	size_t len = strlen(word);
	for (const char* hit = hay; (hit = strstr(hit, word)) != NULL; hit++)
	{
		bool left = hit == hay || !isalnum((unsigned char)hit[-1]);
		bool right = !isalnum((unsigned char)hit[len]);
		if (left && right) return true;
	}
	return false;
}

PlayMode app_title_mode(const char* title)
{
	char lower[256];
	size_t i = 0;
	for (; title[i] && i < sizeof(lower) - 1; i++) lower[i] = (char)tolower((unsigned char)title[i]);
	lower[i] = 0;
	bool swap = strstr(lower, "crossview") || strstr(lower, "cross view") || strstr(lower, "cross-eye") ||
	            strstr(lower, "cross eye");
	// On its own, "side by side" usually means a comparison video ("4K vs 1080p side by
	// side"), so it only counts when the title also says "3d".
	bool is3d = strstr(lower, "3d") != NULL;
	if (has_word(lower, "sbs") || has_word(lower, "hsbs") || has_word(lower, "fsbs") || has_word(lower, "half sbs") ||
	    has_word(lower, "3d lr") || has_word(lower, "lr 3d") ||
	    (is3d && (strstr(lower, "side by side") || strstr(lower, "side-by-side"))))
		return swap ? MODE_SBS_SWAP : MODE_SBS;
	if (is3d && (strstr(lower, "top bottom") || strstr(lower, "top-bottom") || strstr(lower, "topbottom") ||
	             strstr(lower, "top and bottom") || strstr(lower, "over under") || strstr(lower, "over-under") ||
	             has_word(lower, "tab") || has_word(lower, "htab") || has_word(lower, "ou")))
		return swap ? MODE_TB_SWAP : MODE_TB;
	return swap ? MODE_SBS_SWAP : MODE_2D;
}

const QuickPick quick_picks[QUICK_PICKS] = {
	{ "Movies", "movie trailer", true }, { "Nature", "nature", true },  { "Rides", "roller coaster", true },
	{ "Music", "music video", false },   { "Gaming", "gaming", false }, { "Trailers", "official trailer", false },
};

const char* app_layout(const VideoEntry* e)
{
	PlayMode mode = app.mode != MODE_AUTO ? app.mode : app_title_mode(e->title);
	if (mode == MODE_SBS || mode == MODE_SBS_SWAP) return "sbs";
	if (mode == MODE_TB || mode == MODE_TB_SWAP) return "tb";
	return "2d";
}
