#pragma once
// Everything the player needs from the user interface (ui.c implements it with citro2d).
#include <3ds.h>
#include <stdbool.h>
#include "yt.h"   // YtComment

typedef struct {
	const char* title;
	u32 pos_ms, dur_ms;
	float buffered_s;
	bool all_loaded, paused, buffering, stereo, audio_ok;
	bool live;                                          // a live stream: a LIVE mark instead of the progress bar
	const char* mode_name;                              // e.g. "3D side-by-side"
	const char* next_mode_name;                         // what X / the mode button switches to
	u32 errors;                                         // decoder errors (shown if not 0)
	u32 linear_free_kb, heap_free_kb, mvd_workbuf_kb;   // memory readout
	bool set_open;          // the playback settings page replaces the remote (START or its button)
	bool smooth_on;         // the Smooth motion setting
	bool stats_on;          // the nerd stats setting
	bool can_switch;        // the video is 3D (or the mode was set by hand): offer other layouts
	bool show_depth;        // playing in 3D: show the depth adjustment line
	s8 depth_step;          // D-pad Up/Down setting (+ = closer)
	s8 auto_px;             // screen pixels the auto-convergence moved the picture by
	const char* cc_label;   // subtitles button text ("CC Off", "CC en", ...), or NULL for none
	bool cc_on;             // a subtitle track is selected (button highlighted)
	bool cc_none;           // the video has no subtitles (button greyed, "No CC")
	// The subtitle language list. While it's open it replaces the remote; the video keeps playing.
	bool cc_menu;
	const char* const* cc_names;   // cc_count track names (UTF-8)
	int cc_count;
	int cc_hl;    // highlighted row: 0 = Off, 1 + track index
	int cc_sel;   // row in use (same numbering)
	int cc_top;   // first visible row
	// A file's audio tracks, when it has more than one. They follow the subtitles in the same
	// list, so audio track i is row 1 + cc_count + i.
	const char* const* audio_names;
	int audio_count;   // 0: no audio rows
	int audio_sel;     // the one playing
	// Shown under the title: the channel and view count (skipped when NULL or ""). For YouTube
	// videos (comments_ok) a row of buttons follows: likes and dislikes with their counts ("" or
	// NULL while unknown; tapping them only explains that voting needs an account) and Comments,
	// which makes pui_touch return KEY_SELECT, the same as the button.
	const char* channel;
	const char* views;
	bool comments_ok;
	const char* likes;
	const char* dislikes;
	// The comments panel, which also replaces the remote while open.
	bool cm_open;
	const YtComment* cm;   // cm_count of them
	int cm_count;
	bool cm_loading;        // a page is on its way
	const char* cm_error;   // why there are none (or NULL)
} PlayerStatus;

#define PUI_CC_ROWS 5   // rows of the language list on screen at once

// Draws one frame of the loading screen shown while the video is being fetched. Returns true
// if the user tapped Cancel.
bool pui_loading(const char* line1, const char* line2, int tick);

// Called a few times a second during playback to draw only the bottom screen, since the top
// screen belongs to the video. Never blocks.
void pui_status(const PlayerStatus* st);

// Turns touches on the playback screen into button presses (KEY_A, KEY_B, ..., with KEY_Y for
// the subtitles button). The progress bar can be tapped or dragged. While the stylus is on it,
// the knob and time follow along (see pui_scrubbing); when it's lifted, *seek_ms gets the
// target. Otherwise *seek_ms is -1.
u32 pui_touch(u32 dur_ms, s64* seek_ms);
bool pui_scrubbing(void);   // the seek bar is being dragged: redraw the remote every frame

// The comments panel scrolls when dragged with the stylus (handled in pui_touch) or when the
// player feeds it dy pixels a frame while Up/Down is held. want_more says we're within a screen
// or two of the end, so the next page should load. active means it's still moving and needs a
// redraw every frame. reset scrolls back to the top for a new video.
void pui_comments_scroll(float dy);
bool pui_comments_want_more(void);
bool pui_comments_active(void);
void pui_comments_reset(void);

// While the language list is open (the last status drawn had cc_menu), returns the row the
// last pui_touch hit, numbered like cc_hl, or -1 if none. In this state pui_touch returns
// KEY_B for Close and KEY_DUP/KEY_DDOWN for the arrow buttons that move the highlight.
int pui_cc_tapped(void);
// What the last touch on the settings page asked for: PUI_SET_NONE, one of the others, or
// PUI_SET_DEPTH + step + PUI_DEPTH_STEPS for a depth step (-PUI_DEPTH_STEPS..PUI_DEPTH_STEPS).
// Tapping Close makes pui_touch return KEY_B.
enum { PUI_SET_NONE = -1, PUI_SET_CC, PUI_SET_LAYOUT, PUI_SET_SMOOTH, PUI_SET_STATS, PUI_SET_DEPTH = 100 };
#define PUI_DEPTH_STEPS 6
int pui_settings_tapped(void);

// ---- video: the decoded picture is a GPU texture that the UI draws once per eye

typedef enum { VL_2D = 0, VL_SBS, VL_TB } VideoLayout;

typedef struct {
	void* tex;   // C3D_Tex* holding the picture (NULL: nothing decoded yet)
	// For Smooth motion we draw a moment between tex and the next picture, tex2. With motion
	// vectors (mvx/mvy on an mvw x mvh mesh, vertices mcell picture pixels apart), tex is moved
	// forward by mix * v and tex2 is moved back by (1 - mix) * v and blended over it at opacity
	// mix. Without vectors, tex2 is just blended over tex at opacity mix.
	void* tex2;         // or NULL: tex alone
	float mix;          // 0..1: how far the moment is from tex to tex2
	const float* mvx;   // or NULL
	const float* mvy;
	int mvw, mvh;
	float mcell;
	u16 tex_w, tex_h;   // texture size (power of two)
	u16 coded_h;        // picture rows stored in the texture, from its top (v = 1) down
	u16 img_w, img_h;   // visible picture size (top-left of the coded picture)
	VideoLayout layout;
	bool swap_eyes;           // cross-eyed videos: right eye picture is on the left/top
	bool full;                // full-size SBS/TB: each half already has the right aspect
	bool paused, buffering;   // shown on top of the picture, in both eyes
	const char* note;         // under the buffering spinner, e.g. a seek's target time (or NULL)
	const char* subtitle;     // current subtitle text, UTF-8, '\n' between lines (or NULL)
	const char* stats;        // "Nerd stats" lines for the top-right corner, '\n'-separated (or NULL)
	float conv;               // convergence: how many picture pixels right of the left eye's window
	                          // the right eye's sits (positive brings the scene closer; see video_eye)
} PlayerVideoFrame;

// Draws the video on the top screen (left eye, and right eye when the 3D slider is up)
// and, if st is not NULL, the playback remote on the bottom screen, in one GPU frame.
// It draws immediately, so when called right after a top-screen refresh the picture appears at
// the next one.
void pui_video(const PlayerVideoFrame* v, const PlayerStatus* st);

// For GPU work outside of drawing, like the player's picture copies. pui_gpu_begin waits for
// the GPU to finish everything queued earlier (up to timeout_ms, after which it recovers a
// stuck queue; see ui.c) and opens a citro3d frame. C3D_Sync* calls made inside are then
// queued in order rather than each waiting for a transfer event, and pui_gpu_end submits them.
// To wait for them to finish, do another begin/end pair.
bool pui_gpu_begin(u32 timeout_ms);
void pui_gpu_end(void);
u32 pui_gpu_recoveries(void);   // stuck GPU queues recovered so far
bool pui_gpu_dead(void);        // a wait gave up: the GPU is hung and nothing more can be drawn

// Blocking message on the bottom screen. Returns true for the A/yes choice.
// yes_label may be NULL for a single button labelled no_label (returns false).
bool pui_message(const char* title, const char* msg, const char* yes_label, const char* no_label);
