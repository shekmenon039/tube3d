#pragma once
#include <3ds.h>
#include <stdbool.h>
#include "wifi.h"

typedef enum {
	UA_NONE,
	UA_SEARCH,
	UA_SEARCH_AGAIN,
	UA_PREV,
	UA_NEXT,
	UA_PLAY,
	UA_TAB_HOME,
	UA_TAB_PICKS,
	UA_TAB_HISTORY,
	UA_TAB_FILES,
	UA_TAB_SETTINGS,
	UA_WIFI,
	UA_MENU_DEPTH,
	UA_SUBTITLES,
	UA_MOTION,
	UA_NERD_STATS,
	UA_FILES_UP,   // Videos tab: go to the parent folder (B does the same)
	UA_FILES_PGUP,
	UA_FILES_PGDN,   // page the list up or down
	UA_FILES_ROW_0,
	UA_FILES_ROW_1,
	UA_FILES_ROW_2,
	UA_FILES_ROW_3,
	UA_FILES_ROW_4,   // row r on screen, i.e. entry ui_files_first() + r
	UA_FILES_MORE_0,
	UA_FILES_MORE_1,
	UA_FILES_MORE_2,
	UA_FILES_MORE_3,
	UA_FILES_MORE_4,   // that row's ⋮ menu (edit or remove a saved place)
	UA_SET_PAGE_0,
	UA_SET_PAGE_1,
	UA_SET_PAGE_2,   // Settings pages: 3D | Playback | More
	UA_DEPTH_0,
	UA_DEPTH_1,
	UA_DEPTH_2,
	UA_DEPTH_3,   // Menu 3D: Off | Low | Normal | Strong
	UA_PICK_0,
	UA_PICK_1,
	UA_PICK_2,
	UA_PICK_3,
	UA_PICK_4,
	UA_PICK_5,   // quick_picks[i]
	UA_MODE_0,   // UA_MODE_0 + PlayMode
} UiAction;

bool ui_init(void);
void ui_exit(void);

// Call with true when a video starts, to switch to screen-size render targets, and with false
// when it ends, to go back to the menus' 2x2 supersampled ones. See create_targets in ui.c.
void ui_playback(bool playing);
bool ui_supersampled(void);   // for the log
bool ui_font_ok(void);        // for the log: whether the crisp UI font loaded

// Draws one frame of the browsing UI. The top screen is rendered once per eye
// whenever the 3D slider is up (depth scales with the slider) and flat when down.
void ui_draw(void);

// Maps a touch on the bottom screen to an action.
UiAction ui_hit(int x, int y);

// Full-screen notice that waits for START (e.g. "needs a New 3DS").
void ui_fatal(const char* title, const char* msg);

// The Wi-Fi test in Settings: draws one frame of the connection checklist. connecting is true
// while we're asking the console to connect to its saved networks.
void ui_wifi(const WifiTest* t, bool connecting);

// Touch on the Wi-Fi test screen: 1 = Back, 2 = Connect / Test again, 0 = nothing.
int ui_wifi_hit(int x, int y);

// Draws one frame while a network place in the Videos tab is connecting (the connection runs
// elsewhere). The top screen keeps showing the chosen place; the bottom one says "Connecting"
// and where to. Returns true if Cancel was tapped.
bool ui_connecting(const char* where);

// A form page on the bottom screen, used to add a network place: a title, one row per field
// (tap a row, or use Up/Down and A, to type into it), then Cancel and Save. An empty value
// shows the placeholder in grey, and secret values are masked. can_save lights up Save once the
// required fields are filled.
typedef struct {
	const char* label;
	const char* value;
	const char* placeholder;
	bool secret;
} UiField;
#define UI_FORM_MAX 6
void ui_form(const char* title, const UiField* f, int n, int sel, bool can_save);
// Touch on the form: 1 = Cancel, 2 = Save, 100 + i = field i, 0 = nothing.
int ui_form_hit(int x, int y, int n);

// A short menu page, such as a saved place's ⋮ menu: a title, a line under it, n choices and
// Cancel. The choice at index danger is drawn in red (e.g. Remove).
// ui_menu_hit returns 1 for Cancel, 100 + i for choice i, 0 for nothing.
void ui_menu(const char* title, const char* sub, const char* const* items, int n, int sel, int danger);
int ui_menu_hit(int x, int y, int n);

// Index of the first entry visible in the Videos tab list, to turn UA_FILES_ROW_0 + r into
// an entry.
int ui_files_first(void);
