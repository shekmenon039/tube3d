// Tube3D user interface, drawn on the GPU with citro2d.
//
// Top screen: a cover-flow carousel. Every element sits at a depth; when the
// 3D slider is up the scene is drawn once per eye with each layer shifted
// horizontally by (depth x slider), so the selected card floats in front of the
// screen and the background sits behind it. Slider down = one flat image.
// 3D videos get a thumbnail per eye, so their cards show real 3D in the menu.
//
// Bottom screen: touch controls (search, prev/next, play, tabs), and during
// playback a remote with a progress bar you can tap or drag to seek.

#include "ui.h"
#include "app.h"
#include "playerui.h"
#include "power.h"
#include "thumbs.h"
#include "uifont.h"
#include "logo.h"
#include "vmesh.h"
#include "files.h"
#include "filethumb.h"

#include <citro2d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define UI_MAX_OBJECTS 2560

#define RGBA(r, g, b, a) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | ((u32)(a) << 24))

static const u32 C_BG = RGBA(11, 13, 18, 255);
static const u32 C_BG2 = RGBA(17, 19, 24, 255);
static const u32 C_PANEL = RGBA(31, 35, 43, 255);
static const u32 C_PANEL2 = RGBA(46, 51, 61, 255);
static const u32 C_RED = RGBA(255, 71, 87, 255);
static const u32 C_CYAN = RGBA(0, 210, 211, 255);
static const u32 C_CYANDK = RGBA(4, 52, 44, 255);
static const u32 C_WHITE = RGBA(255, 255, 255, 255);
static const u32 C_MUTED = RGBA(154, 165, 177, 255);
static const u32 C_AMBER = RGBA(255, 196, 0, 255);

#define TOP_W       400   // the top screen
#define TOP_H       240
#define BOT_W       320   // the bottom screen
#define BOT_H       240
#define TOP_CX      (TOP_W / 2)   // the screens' horizontal centres
#define BOT_CX      (BOT_W / 2)
#define THUMB_W     224.0f   // a thumbnail's picture, in the top-left of its THUMB_TEX_W wide texture
#define THUMB_H     126.0f
#define THUMB_TEX_W 256.0f
#define TWO_PI      6.2831853f
#define DEG_TO_RAD  0.0174533f

static C3D_RenderTarget *top_l, *top_r, *bot;
static C2D_TextBuf frame_text;
static C2D_TextBuf measure_text;   // only for text_w, so measuring never eats into the frame's glyphs (frame_text)
static void begin_scene(C3D_RenderTarget* t);   // C2D_SceneBegin for our targets (see create_targets)

// the eye being drawn: +1 left, -1 right, 0 flat (slider down)
static float g_eye = 0.0f;
static float g_depth_px = 0.0f;
#define DX(d) (g_eye * (d) * g_depth_px)

static float bg_r = 11, bg_g = 13, bg_b = 18;   // animated background colour

// ---------------------------------------------------------------- small helpers

static u32 with_alpha(u32 color, float alpha)
{
	if (alpha < 0) alpha = 0;
	if (alpha > 1) alpha = 1;
	return (color & 0x00FFFFFF) | ((u32)(((color >> 24) & 0xFF) * alpha) << 24);
}

static void rrect(float x, float y, float w, float h, float r, u32 c)
{
	if (w <= 0 || h <= 0) return;
	if (r * 2 > h) r = h / 2;
	if (r * 2 > w) r = w / 2;
	if ((c >> 24) < 255)
	{
		// translucent: overlapping corner circles would show darker spots, so
		// build it from non-overlapping pieces with bevelled corners instead
		C2D_DrawRectSolid(x, y + r, 0.5f, w, h - 2 * r, c);
		C2D_DrawRectSolid(x + r, y, 0.5f, w - 2 * r, r, c);
		C2D_DrawRectSolid(x + r, y + h - r, 0.5f, w - 2 * r, r, c);
		C2D_DrawTriangle(x, y + r, c, x + r, y, c, x + r, y + r, c, 0.5f);
		C2D_DrawTriangle(x + w - r, y, c, x + w, y + r, c, x + w - r, y + r, c, 0.5f);
		C2D_DrawTriangle(x, y + h - r, c, x + r, y + h - r, c, x + r, y + h, c, 0.5f);
		C2D_DrawTriangle(x + w - r, y + h - r, c, x + w, y + h - r, c, x + w - r, y + h, c, 0.5f);
		return;
	}
	C2D_DrawRectSolid(x + r, y, 0.5f, w - 2 * r, h, c);
	C2D_DrawRectSolid(x, y + r, 0.5f, r, h - 2 * r, c);
	C2D_DrawRectSolid(x + w - r, y + r, 0.5f, r, h - 2 * r, c);
	C2D_DrawCircleSolid(x + r, y + r, 0.5f, r, c);
	C2D_DrawCircleSolid(x + w - r, y + r, 0.5f, r, c);
	C2D_DrawCircleSolid(x + r, y + h - r, 0.5f, r, c);
	C2D_DrawCircleSolid(x + w - r, y + h - r, 0.5f, r, c);
}

static void tri_right(float x, float y, float s, u32 c)
{
	C2D_DrawTriangle(x, y - s, c, x, y + s, c, x + s * 1.6f, y, c, 0.5f);
}

static void tri_left(float x, float y, float s, u32 c)
{
	C2D_DrawTriangle(x, y - s, c, x, y + s, c, x - s * 1.6f, y, c, 0.5f);
}

// centred on (x, y)
static void tri_up(float x, float y, float s, u32 c)
{
	C2D_DrawTriangle(x - s, y + s * 0.6f, c, x + s, y + s * 0.6f, c, x, y - s * 0.8f, c, 0.5f);
}

static void tri_down(float x, float y, float s, u32 c)
{
	C2D_DrawTriangle(x - s, y - s * 0.6f, c, x + s, y - s * 0.6f, c, x, y + s * 0.8f, c, 0.5f);
}

static C2D_Text parse_text(const char* s)
{
	C2D_Text t;
	C2D_TextParse(&t, frame_text, s ? s : "");
	C2D_TextOptimize(&t);
	return t;
}

// Text goes through uifont (pre-rendered Noto Sans drawn 1:1 at whole pixels, so it's sharp)
// and falls back to the system font for strings with characters it lacks (Japanese, emoji...).
static int uifont_align(u32 align)
{
	u32 a = align & C2D_AlignMask;
	return a == C2D_AlignCenter ? UIF_CENTER : a == C2D_AlignRight ? UIF_RIGHT : UIF_LEFT;
}

static void text(const char* s, float x, float y, float scale, u32 color, u32 align)
{
	if (uifont_draw(s ? s : "", x, y, 0.5f, scale, color, uifont_align(align))) return;
	C2D_Text t = parse_text(s);
	C2D_DrawText(&t, C2D_WithColor | align, x, y, 0.5f, scale, scale, color);
}

static void text_wrap(const char* s, float x, float y, float scale, u32 color, u32 align, float width)
{
	if (uifont_draw_wrap(s ? s : "", x, y, 0.5f, scale, color, uifont_align(align), width)) return;
	C2D_Text t = parse_text(s);
	C2D_DrawText(&t, C2D_WithColor | C2D_WordWrap | align, x, y, 0.5f, scale, scale, color, width);
}

static float text_w(const char* s, float scale)
{
	float w = 0, h = 0;
	if (uifont_width(s ? s : "", scale, &w)) return w;
	// uifont lacks a character, so measure with the system font in a buffer of its own
	C2D_Text t;
	C2D_TextBufClear(measure_text);
	C2D_TextParse(&t, measure_text, s ? s : "");
	C2D_TextGetDimensions(&t, scale, scale, &w, &h);
	return w;
}

// Copies at most maxbytes of UTF-8 without splitting a character; adds "..." if cut.
static void clip(char* out, size_t cap, const char* s, size_t maxbytes)
{
	size_t n = strlen(s);
	if (n <= maxbytes && n < cap)
	{
		memcpy(out, s, n + 1);
		return;
	}
	if (maxbytes > cap - 4) maxbytes = cap - 4;
	while (maxbytes > 0 && ((unsigned char)s[maxbytes] & 0xC0) == 0x80) maxbytes--;
	while (maxbytes > 0 && s[maxbytes - 1] == ' ') maxbytes--;
	memcpy(out, s, maxbytes);
	memcpy(out + maxbytes, "...", 4);
}

static void fmt_dur(char* out, size_t len, int sec)
{
	if (sec <= 0) snprintf(out, len, "LIVE");
	else if (sec >= 3600) snprintf(out, len, "%d:%02d:%02d", sec / 3600, (sec / 60) % 60, sec % 60);
	else snprintf(out, len, "%d:%02d", sec / 60, sec % 60);
}

static bool in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
	return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static u32 hash_color(const char* id)
{
	u32 hash = 2166136261u;
	for (; *id; id++) hash = (hash ^ (u8)*id) * 16777619u;
	return RGBA(40 + (hash & 63), 40 + ((hash >> 8) & 63), 60 + ((hash >> 16) & 79), 255);
}

// the selected entry of a tab's list of n, which may have shrunk under it
#define SELECTED(tab, n) (app.sel[tab] < (n) ? app.sel[tab] : (n) - 1)

static float now_s(void) { return (float)(app.frame % 100000) / 60.0f; }

static void spinner(float cx, float cy, float r, u32 c)
{
	float phase = now_s() * 12.0f;
	for (int dot = 0; dot < 10; dot++)
	{
		float angle = dot * TWO_PI / 10.0f;
		float fade = fmodf((float)dot - phase + 1000.0f, 10.0f) / 10.0f;
		C2D_DrawCircleSolid(cx + cosf(angle) * r, cy + sinf(angle) * r, 0.5f, r * 0.2f,
		                    with_alpha(c, 0.2f + 0.8f * fade));
	}
}

// ---------------------------------------------------------------- background + header

static void background(void)
{
	u32 bg = RGBA((u8)bg_r, (u8)bg_g, (u8)bg_b, 255);
	C2D_DrawRectSolid(0, 0, 0.5f, TOP_W, TOP_H, bg);
	// slow drifting orbs far behind the screen
	float secs = now_s();
	for (int orb = 0; orb < 3; orb++)
	{
		float cx = 70 + orb * 130 + sinf(secs * 0.3f + orb) * 22;
		float cy = 70 + (orb % 2) * 100 + cosf(secs * 0.23f + orb * 2) * 16;
		u32 orb_color = RGBA((u8)fminf(bg_r + 40, 255), (u8)fminf(bg_g + 40, 255), (u8)fminf(bg_b + 50, 255), 70);
		C2D_DrawCircleSolid(cx + DX(-3.0f), cy, 0.5f, 78 - orb * 12, orb_color);
	}
}

// Wi-Fi fan: a dot and three arcs above it, lit up to the signal strength (0-3).
// (cx, by) is the dot. Offline: dim arcs and a red dot.
static void wifi_icon(float cx, float by, int bars, bool connected)
{
	const u32 on = C_WHITE, off = RGBA(255, 255, 255, 60);
	C2D_DrawCircleSolid(cx, by, 0.5f, 1.7f, connected ? on : C_RED);
	for (int bar = 1; bar <= 3; bar++)
	{
		float radius = 3.4f * bar + 0.6f;
		u32 color = connected && bars >= bar ? on : off;
		for (int seg = 0; seg < 5; seg++)   // arc from up-left to up-right in 5 segments
		{
			float a0 = (-135.0f + 18.0f * seg) * DEG_TO_RAD, a1 = (-135.0f + 18.0f * (seg + 1)) * DEG_TO_RAD;
			C2D_DrawLine(cx + cosf(a0) * radius, by + sinf(a0) * radius, color, cx + cosf(a1) * radius,
			             by + sinf(a1) * radius, color, 1.7f, 0.5f);
		}
	}
}

// Battery: outline, fill for the charge (amber at 20% and below, red at 10%), a bolt while
// charging, and the percentage to its left. (right, top) is the outline's top-right corner.
static void battery_icon(float right, float top)
{
	const float body_w = 19, body_h = 10;
	int pct = power_percent();
	bool charging = power_charging();
	float x = right - body_w;
	u32 fill = charging ? C_CYAN : pct <= 10 ? C_RED : pct <= 20 ? C_AMBER : RGBA(46, 213, 115, 255);
	rrect(x, top, body_w, body_h, 2, C_MUTED);   // outline
	C2D_DrawRectSolid(x + 1.2f, top + 1.2f, 0.5f, body_w - 2.4f, body_h - 2.4f, C_BG);
	C2D_DrawRectSolid(right, top + 3, 0.5f, 2, 4, C_MUTED);   // terminal
	float fill_w = (body_w - 3.6f) * (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100.0f;
	if (fill_w > 0) C2D_DrawRectSolid(x + 1.8f, top + 1.8f, 0.5f, fill_w, body_h - 3.6f, fill);
	if (charging)
	{
		// small lightning bolt across the middle
		float cx = x + body_w / 2;
		C2D_DrawTriangle(cx + 1.5f, top + 1, C_WHITE, cx - 2.5f, top + 5.5f, C_WHITE, cx + 0.5f, top + 5.5f, C_WHITE,
		                 0.5f);
		C2D_DrawTriangle(cx - 1.5f, top + 9, C_WHITE, cx + 2.5f, top + 4.5f, C_WHITE, cx - 0.5f, top + 4.5f, C_WHITE,
		                 0.5f);
	}
	char label[8];
	snprintf(label, sizeof(label), "%d%%", pct);
	text(label, x - 3, top - 2, 0.42f, C_MUTED, C2D_AlignRight);
}

// The top screen's header: the small logo, then the clock, Wi-Fi and battery on the right.
// Screens that show the big logo pass logo = false.
static void header(bool logo)
{
	// the "3D" floats a little in front of the rest of the logo
	if (logo && !logo_draw(LOGO_HEADER, 10, 3, DX(0.8f), DX(1.8f)))
	{
		float x = 10 + DX(0.8f);
		tri_right(x, 13, 6, C_RED);
		text("tube", x + 14, 4, 0.55f, C_WHITE, C2D_AlignLeft);
		text("3D", x + 14 + text_w("tube", 0.55f), 4, 0.55f, C_CYAN, C2D_AlignLeft);
	}

	char clock_text[16];
	time_t now = time(NULL);
	struct tm* local_time = localtime(&now);
	snprintf(clock_text, sizeof(clock_text), "%02d:%02d", local_time ? local_time->tm_hour : 0,
	         local_time ? local_time->tm_min : 0);
	float clock_w = text_w(clock_text, 0.5f);
	text(clock_text, 392 + DX(0.8f), 5, 0.5f, C_MUTED, C2D_AlignRight);
	float wifi_x = 392 - clock_w - 17;   // the Wi-Fi arcs reach ~11 px right of their centre
	wifi_icon(wifi_x + DX(0.8f), 18, wifi_bars(), wifi_connected());
	battery_icon(wifi_x - 14 + DX(0.8f), 8);
}

// ---------------------------------------------------------------- cards

// Draws one eye's thumbnail into a w x h card. For 3D thumbnails (disp != 0) each eye shows
// a window of its picture offset by half the measured disparity, from opposite sides, so
// the picture's main subject lands at the card's depth instead of adding the video's own
// depth on top (which the eyes can't fuse at thumbnail size). The window is stretched back
// to the full card, a zoom of at most ~10%.
static void thumb_eye(C2D_Image img, float x, float y, float w, float h, int disp, const C2D_ImageTint* tint)
{
	if (disp == 0 || g_eye == 0)
	{
		C2D_DrawImageAt(img, x, y, 0.5f, tint, w / THUMB_W, h / THUMB_H);
		return;
	}
	float half = disp / 2.0f;
	float margin = half < 0 ? -half : half;   // crop that keeps both windows inside the picture
	float win_w = THUMB_W - 2 * margin;
	float win_x = margin + (g_eye > 0 ? -half : half);   // left eye window moves back, right eye forward
	Tex3DS_SubTexture sub = *img.subtex;
	sub.width = (u16)win_w;
	sub.left = win_x / THUMB_TEX_W;
	sub.right = (win_x + win_w) / THUMB_TEX_W;
	C2D_Image win = { img.tex, &sub };
	C2D_DrawImageAt(win, x, y, 0.5f, tint, w / win_w, h / THUMB_H);
}

static void badge(float x, float y, const char* label, bool is3d, float scale)
{
	float w = text_w(label, scale) + 10;
	rrect(x, y, w, 15 * scale / 0.45f, 4, is3d ? C_CYAN : C_PANEL2);
	text(label, x + 5, y + 1, scale, is3d ? C_CYANDK : C_WHITE, C2D_AlignLeft);
}

// carousel layout: the selected card is CARD_W x CARD_H, centred at CAROUSEL_X; the cards beside it
// sit CARD_NEAR_DX away and each one further out CARD_FAR_DX more
#define CAROUSEL_X   TOP_CX
#define CARD_W       208
#define CARD_H       117
#define CARD_NEAR_DX 152
#define CARD_FAR_DX  92

// How far from CAROUSEL_X a card's centre sits, dist cards from the selected one
static float card_shift(float dist)
{
	const float side_t = dist < 1 ? dist : 1;
	return side_t * CARD_NEAR_DX + (dist > 1 ? (dist - 1) * CARD_FAR_DX : 0);
}

// offset: from the centre of the carousel (0 = selected card)
static void card(const VideoEntry* e, float offset)
{
	float dist = fabsf(offset);
	float side_t = dist < 1 ? dist : 1;
	float scale = 1.0f - 0.36f * side_t - (dist > 1 ? 0.1f * (dist - 1) : 0);
	if (scale < 0.35f) scale = 0.35f;
	float w = CARD_W * scale, h = CARD_H * scale;
	float side = offset < 0 ? -1.0f : 1.0f;
	float cx = CAROUSEL_X + side * card_shift(dist);
	float cy = 94 + side_t * 8;
	float alpha = dist <= 1 ? 1.0f - 0.35f * side_t : 0.65f - 0.55f * (dist - 1);
	if (alpha <= 0.02f) return;
	float depth = 4.0f - 3.0f * side_t - (dist > 1 ? 0.5f * (dist - 1) : 0);   // selected card pops out most
	float x = cx - w / 2 + DX(depth), y = cy - h / 2;

	const char* layout = app_layout(e);
	bool is3d = strcmp(layout, "2d") != 0;

	if (dist < 0.5f)
	{
		// shadow slightly behind the floating card, then a white frame
		rrect(cx - w / 2 + 6 + DX(depth - 1.5f), y + 7, w, h, 8, RGBA(0, 0, 0, 110));
		rrect(x - 3, y - 3, w + 6, h + 6, 9, with_alpha(C_WHITE, alpha));
	}

	C2D_Image left_img, right_img;
	int disp = 0;
	if (thumbs_get(e->id, layout, &left_img, &right_img, NULL, &disp))
	{
		C2D_ImageTint tint;
		C2D_AlphaImageTint(&tint, alpha);
		thumb_eye(g_eye < 0 ? right_img : left_img, x, y, w, h, disp, &tint);
	}
	else
	{
		float pulse = 0.75f + 0.25f * sinf(now_s() * 4 + (float)(e->id[0]));
		rrect(x, y, w, h, 6, with_alpha(hash_color(e->id), alpha * pulse));
		tri_right(cx - 8 * scale + DX(depth + 0.5f), cy, 12 * scale, with_alpha(C_WHITE, alpha * 0.8f));
	}

	if (dist < 1.5f)
	{
		if (is3d) badge(x + 6 + DX(0.8f), y + 6, "3D", true, 0.42f * (0.6f + 0.4f * scale));
		char dur[16];
		fmt_dur(dur, sizeof(dur), e->duration);
		float dur_w = text_w(dur, 0.42f * scale) + 8;
		// the length, or a red LIVE for a live stream
		rrect(x + w - dur_w - 5, y + h - 18 * scale - 4, dur_w, 16 * scale + 2, 3,
		      with_alpha(e->duration > 0 ? RGBA(0, 0, 0, 190) : C_RED, alpha));
		text(dur, x + w - dur_w - 1, y + h - 18 * scale - 3, 0.42f * scale, with_alpha(C_WHITE, alpha), C2D_AlignLeft);
	}
}

static void carousel(VideoEntry* list, int n, Tab tab)
{
	float pos = app.anim[tab];
	int first_idx = (int)floorf(pos) - 3, last_idx = (int)ceilf(pos) + 3;
	if (first_idx < 0) first_idx = 0;
	if (last_idx > n - 1) last_idx = n - 1;
	// farthest first so the selected card is drawn on top
	int order[16], count = 0;
	for (int i = first_idx; i <= last_idx && count < 16; i++) order[count++] = i;
	for (int i = 0; i < count; i++)
		for (int j = i + 1; j < count; j++)
			if (fabsf(order[j] - pos) > fabsf(order[i] - pos))
			{
				int tmp = order[i];
				order[i] = order[j];
				order[j] = tmp;
			}
	for (int i = 0; i < count; i++) card(&list[order[i]], order[i] - pos);
	// the next page on its way: a spinner where the card after the last one will be
	const float end_offset = n - pos;
	if (app.loading_more && end_offset < 2.5f)
		spinner(CAROUSEL_X + card_shift(end_offset) + DX(1.0f), 102, 12, C_MUTED);
}

static void info_strip(const VideoEntry* e, int index, int n)
{
	C2D_DrawRectSolid(0, 170, 0.5f, 400, 70, RGBA(0, 0, 0, 150));
	const char* layout = app_layout(e);
	bool is3d = strcmp(layout, "2d") != 0;
	const char* badge_label = !is3d ? "2D" : !strcmp(layout, "tb") ? "3D TB" : "3D SBS";
	float badge_x = 12 + DX(2.4f);
	badge(badge_x, 177, badge_label, is3d, 0.45f);

	// channel  ·  views  ·  length
	char dur[16], meta[160], channel_shown[48];
	fmt_dur(dur, sizeof(dur), e->duration);
	clip(channel_shown, sizeof(channel_shown), e->channel[0] ? e->channel : "Unknown", e->views[0] ? 24 : 44);
	snprintf(meta, sizeof(meta), "%s  \xC2\xB7  %s%s%s", channel_shown, e->views[0] ? e->views : "",
	         e->views[0] ? "  \xC2\xB7  " : "", dur);
	char meta_shown[96];
	clip(meta_shown, sizeof(meta_shown), meta, 72);
	text(meta_shown, badge_x + text_w(badge_label, 0.45f) + 18, 177, 0.45f, C_MUTED, C2D_AlignLeft);

	char pos[24];
	snprintf(pos, sizeof(pos), "%d / %d", index + 1, n);
	text(pos, 390 + DX(2.4f), 177, 0.45f, C_MUTED, C2D_AlignRight);

	char title[128];
	clip(title, sizeof(title), e->title, 100);
	text_wrap(title, 12 + DX(1.8f), 196, 0.6f, C_WHITE, C2D_AlignLeft, 376);
}

// ---------------------------------------------------------------- top screen scenes

// ---- logos for the Videos tab's sources. They're drawn from shapes so they stay sharp at any
// size and get depth like everything else. Each fills an s x s box at (x, y).

// SD card: the card's outline with its cut corner, gold contacts, "SD"
static void logo_sd(float x, float y, float s)
{
	const u32 body = RGBA(36, 92, 178, 255), edge = RGBA(70, 130, 220, 255), gold = RGBA(232, 186, 74, 255);
	const float w = s * 0.74f, h = s * 0.94f, x0 = x + (s - w) / 2, y0 = y + (s - h) / 2, cut_size = s * 0.24f;
	// the outline a little bigger, then the body: a rectangle with its top-right corner cut
	for (int pass = 0; pass < 2; pass++)
	{
		const float grow = pass ? 0 : s * 0.035f;
		const u32 col = pass ? body : edge;
		C2D_DrawRectSolid(x0 - grow, y0 + cut_size - grow, 0.5f, w + 2 * grow, h - cut_size + 2 * grow, col);
		C2D_DrawRectSolid(x0 - grow, y0 - grow, 0.5f, w - cut_size + 2 * grow, cut_size + grow, col);
		C2D_DrawTriangle(x0 + w - cut_size + grow, y0 - grow, col, x0 + w + grow, y0 + cut_size - grow, col,
		                 x0 + w - cut_size + grow, y0 + cut_size - grow, col, 0.5f);
	}
	// contacts along the top
	const float contact_w = w * 0.11f, contact_h = h * 0.2f;
	for (int i = 0; i < 4; i++)
		C2D_DrawRectSolid(x0 + w * 0.1f + i * contact_w * 1.45f, y0 + h * 0.07f, 0.5f, contact_w, contact_h, gold);
	if (s >= 30) text("SD", x0 + w / 2, y0 + h * 0.48f, s / 70.0f, C_WHITE, C2D_AlignCenter);
	else C2D_DrawRectSolid(x0 + w * 0.2f, y0 + h * 0.62f, 0.5f, w * 0.6f, h * 0.12f, RGBA(255, 255, 255, 200));
}

// SMB (a Windows / NAS share): a storage box of two drive units with their status lights
static void logo_smb(float x, float y, float s, bool active)
{
	const u32 unit = active ? RGBA(74, 82, 98, 255) : RGBA(60, 64, 74, 255);
	const u32 led = active ? RGBA(80, 220, 120, 255) : RGBA(110, 116, 128, 255);
	const float w = s * 0.92f, unit_h = s * 0.4f, x0 = x + (s - w) / 2, gap = s * 0.08f;
	for (int i = 0; i < 2; i++)
	{
		const float unit_y = y + (s - 2 * unit_h - gap) / 2 + i * (unit_h + gap);
		rrect(x0, unit_y, w, unit_h, s * 0.08f, unit);
		C2D_DrawRectSolid(x0 + w * 0.1f, unit_y + unit_h * 0.42f, 0.5f, w * 0.46f, unit_h * 0.16f,
		                  RGBA(30, 34, 42, 255));   // the bay
		C2D_DrawCircleSolid(x0 + w * 0.8f, unit_y + unit_h / 2, 0.5f, unit_h * 0.14f, led);
		C2D_DrawCircleSolid(x0 + w * 0.9f, unit_y + unit_h / 2, 0.5f, unit_h * 0.14f,
		                    i ? RGBA(80, 170, 240, 255) : led);
	}
}

// SFTP (files over SSH): a terminal window with ">_" and a padlock
static void logo_sftp(float x, float y, float s, bool active)
{
	const u32 win = RGBA(28, 32, 40, 255), bar = active ? RGBA(70, 78, 92, 255) : RGBA(58, 62, 72, 255);
	const u32 prompt = active ? RGBA(90, 230, 140, 255) : RGBA(120, 126, 138, 255);
	const u32 lock = active ? RGBA(232, 186, 74, 255) : RGBA(130, 134, 144, 255);
	const float w = s * 0.94f, h = s * 0.78f, x0 = x + (s - w) / 2, y0 = y + (s - h) / 2;
	rrect(x0, y0, w, h, s * 0.08f, bar);
	rrect(x0 + s * 0.04f, y0 + h * 0.2f, w - s * 0.08f, h * 0.76f, s * 0.05f, win);
	// ">" and "_"
	const float stroke = s >= 30 ? s * 0.06f : 1.5f, prompt_x = x0 + w * 0.14f, prompt_y = y0 + h * 0.36f,
	            prompt_h = h * 0.3f;
	C2D_DrawLine(prompt_x, prompt_y, prompt, prompt_x + prompt_h * 0.55f, prompt_y + prompt_h / 2, prompt, stroke,
	             0.5f);
	C2D_DrawLine(prompt_x + prompt_h * 0.55f, prompt_y + prompt_h / 2, prompt, prompt_x, prompt_y + prompt_h, prompt,
	             stroke, 0.5f);
	C2D_DrawRectSolid(prompt_x + prompt_h * 0.75f, prompt_y + prompt_h - stroke / 2, 0.5f, prompt_h * 0.6f, stroke,
	                  prompt);
	// the padlock, bottom right: a shackle (ring with the window colour inside) over a body
	const float lock_w = s * 0.3f, lock_x = x0 + w - lock_w - s * 0.08f, lock_y = y0 + h * 0.56f;
	C2D_DrawCircleSolid(lock_x + lock_w / 2, lock_y, 0.5f, lock_w * 0.36f, lock);
	C2D_DrawCircleSolid(lock_x + lock_w / 2, lock_y, 0.5f, lock_w * 0.2f, win);
	rrect(lock_x, lock_y, lock_w, lock_w * 0.75f, s * 0.04f, lock);
	C2D_DrawRectSolid(lock_x + lock_w * 0.45f, lock_y + lock_w * 0.22f, 0.5f, lock_w * 0.1f, lock_w * 0.3f, win);
}

// Which logo a Videos tab entry gets: the SD card, SMB (the source or a saved server), SFTP,
// an "Add ..." entry, and none for ordinary folders and files.
enum { PLACE_NONE, PLACE_SD, PLACE_SMB, PLACE_SFTP, PLACE_ADD };

static int place_kind(const VideoEntry* e)
{
	if (!strcmp(e->path, FILES_SD_ROOT)) return PLACE_SD;
	if (!strcmp(e->path, "smb:+") || !strcmp(e->path, "sftp:+")) return PLACE_ADD;
	const bool servers = !strcmp(app.files_dir, "smb:") || !strcmp(app.files_dir, "sftp:");
	if (!strcmp(e->path, "smb:") || (servers && !strncmp(e->path, "smb://", 6))) return PLACE_SMB;
	if (!strcmp(e->path, "sftp:") || (servers && !strncmp(e->path, "sftp://", 7))) return PLACE_SFTP;
	return PLACE_NONE;
}

// A file's channel is like "1.2 GB  ·  mkv": the length of its size part
static int size_len(const char* channel)
{
	const char* size_end = strstr(channel, "  ");
	return (int)(size_end ? size_end - channel : (long)strlen(channel));
}

static void logo_add(float x, float y, float s)
{
	C2D_DrawCircleSolid(x + s / 2, y + s / 2, 0.5f, s * 0.45f, C_PANEL2);
	C2D_DrawRectSolid(x + s * 0.47f, y + s * 0.25f, 0.5f, s * 0.06f + 1, s * 0.5f, C_CYAN);
	C2D_DrawRectSolid(x + s * 0.25f, y + s * 0.47f, 0.5f, s * 0.5f, s * 0.06f + 1, C_CYAN);
}

static void place_logo(int kind, float x, float y, float s)
{
	if (kind == PLACE_SD) logo_sd(x, y, s);
	else if (kind == PLACE_SMB) logo_smb(x, y, s, true);
	else if (kind == PLACE_SFTP) logo_sftp(x, y, s, true);
	else if (kind == PLACE_ADD) logo_add(x, y, s);
}

// Videos tab, top screen: the selected item, shown large. The list itself is on the bottom screen.
static void top_files(void)
{
	background();
	header(true);
	if (!app.files_dir[0])
	{
		// the source list: just a title up here, the tiles are on the bottom screen
		text("Videos", TOP_CX + DX(3), 96, 1.4f, C_WHITE, C2D_AlignCenter);
		return;
	}
	char loc[300];
	files_location(app.files_dir, loc, sizeof(loc));
	char shown[120];
	clip(shown, sizeof(shown), loc, 70);
	text(shown, 12 + DX(1), 26, 0.5f, C_CYAN, C2D_AlignLeft);
	int n;
	VideoEntry* list = app_list(TAB_FILES, &n);
	if (n == 0)
	{
		text("No videos in this folder", TOP_CX + DX(1.5f), 100, 0.6f, C_WHITE, C2D_AlignCenter);
		text("Tube3D plays MKV and MP4 files with H.264 video", TOP_CX + DX(1), 130, 0.45f, C_MUTED, C2D_AlignCenter);
		return;
	}
	const VideoEntry* cur = &list[SELECTED(TAB_FILES, n)];
	char name[200];
	if (cur->folder)
	{
		// a folder or a saved place: its icon and name
		const float icon_s = 96, icon_x = TOP_CX - icon_s / 2 + DX(4), icon_y = 50;
		const int kind = place_kind(cur);
		if (kind) place_logo(kind, icon_x, icon_y, icon_s);
		else
		{
			rrect(icon_x + 6, icon_y + 14, 34, 14, 4, C_AMBER);   // a folder
			rrect(icon_x + 6, icon_y + 22, 84, 62, 8, C_AMBER);
		}
		clip(name, sizeof(name), cur->title, 60);
		text(name, TOP_CX + DX(2), 160, 0.75f, C_WHITE, C2D_AlignCenter);
		if (kind == PLACE_SMB || kind == PLACE_SFTP)
		{
			char info[100];
			clip(info, sizeof(info), cur->channel, 60);   // the server's address
			text(info, TOP_CX + DX(1), 190, 0.5f, C_MUTED, C2D_AlignCenter);
		}
		return;
	}
	if (cur->other)
	{
		// a file that isn't a video: a page icon, its name and size
		const float px = TOP_CX - 36 + DX(4), py = 52;
		rrect(px, py, 72, 92, 8, C_PANEL2);
		for (int i = 0; i < 4; i++) C2D_DrawRectSolid(px + 14, py + 22 + i * 14, 0.5f, i == 3 ? 26 : 44, 4, C_MUTED);
		clip(name, sizeof(name), cur->title, 60);
		text(name, TOP_CX + DX(2), 160, 0.6f, C_WHITE, C2D_AlignCenter);
		char info[100];
		snprintf(info, sizeof(info), "%.40s  \xC2\xB7  not a video", cur->channel);
		text(info, TOP_CX + DX(1), 188, 0.45f, C_MUTED, C2D_AlignCenter);
		return;
	}
	// a video: a frame taken from inside it (filethumb.c), in 3D for side-by-side and top-bottom files
	char id[24];
	filethumb_id(cur->path, id, sizeof(id));
	const float pw = 256, ph = 144, px = TOP_CX - pw / 2 + DX(4), py = 42;
	rrect(px - 3, py - 3, pw + 6, ph + 6, 9, C_PANEL2);
	C2D_Image left_img, right_img;
	int disp = 0;
	if (thumbs_get(id, app_layout(cur), &left_img, &right_img, NULL, &disp))
		thumb_eye(g_eye < 0 ? right_img : left_img, px, py, pw, ph, disp, NULL);
	else
	{
		rrect(px, py, pw, ph, 7, C_PANEL);
		spinner(px + pw / 2 + DX(1), py + ph / 2, 14, C_MUTED);
	}
	if (cur->resume_ms && cur->resume_dur)
	{
		// watched part-way: a progress line along the picture's bottom, the way YouTube marks it
		float watched = (float)cur->resume_ms / (float)cur->resume_dur;
		if (watched > 1) watched = 1;
		C2D_DrawRectSolid(px, py + ph - 5, 0.5f, pw, 5, RGBA(40, 44, 52, 220));
		C2D_DrawRectSolid(px, py + ph - 5, 0.5f, pw * watched, 5, C_RED);
	}
	clip(name, sizeof(name), cur->title, 64);
	text(name, TOP_CX + DX(1.5f), py + ph + 8, 0.52f, C_WHITE, C2D_AlignCenter);
	char line[96] = "";
	if (cur->resume_ms)
	{
		char at[16];
		fmt_dur(at, sizeof(at), (int)(cur->resume_ms / 1000));
		snprintf(line, sizeof(line), "From %s", at);
	}
	u32 dur = 0;
	int vw = 0, vh = 0;
	if (filethumb_info(cur->path, &dur, &vw, &vh))
	{
		char dur_text[16] = "";
		if (dur) fmt_dur(dur_text, sizeof(dur_text), (int)(dur / 1000));
		snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s%s%s%dp",
		         line[0] && dur_text[0] ? "  \xC2\xB7  " : "", dur_text, line[0] ? "  \xC2\xB7  " : "", vh);
	}
	snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s%.*s", line[0] ? "  \xC2\xB7  " : "",
	         size_len(cur->channel), cur->channel);
	const char* layout = app_layout(cur);
	const bool is3d = strcmp(layout, "2d") != 0;
	const char* badge_label = !strcmp(layout, "tb") ? "3D TB" : "3D SBS";
	float line_w = text_w(line, 0.45f), badge_w = is3d ? text_w(badge_label, 0.45f) + 18 : 0;
	float line_x = TOP_CX - (line_w + badge_w) / 2 + DX(1);
	text(line, line_x, py + ph + 30, 0.45f, C_MUTED, C2D_AlignLeft);
	if (is3d) badge(line_x + line_w + 8, py + ph + 29, badge_label, true, 0.45f);
}

static void top_browse(void)
{
	if (app.tab == TAB_FILES)
	{
		top_files();
		return;
	}
	background();
	int n = 0;
	VideoEntry* list = app.tab == TAB_SETTINGS ? NULL : app_list(app.tab, &n);
	header(app.tab == TAB_SETTINGS || n > 0);   // the empty screen shows the big logo instead

	if (app.tab == TAB_SETTINGS)
	{
		text("Settings", TOP_CX + DX(3), 96, 1.4f, C_WHITE, C2D_AlignCenter);   // the options are below
		return;
	}

	if (n == 0)
	{
		// the big logo: the play square and "tube" just in front of the screen, the "3D" further out
		int lw, lh;
		logo_size(LOGO_HERO, &lw, &lh);
		if (!logo_draw(LOGO_HERO, TOP_CX - lw / 2, 66, DX(2.0f), DX(4.0f)))
		{
			float big = 1.4f;
			float w1 = text_w("tube", big), w2 = text_w("3D", big);
			float x = TOP_CX - (w1 + w2) / 2 + DX(3.5f);
			text("tube", x, 68, big, C_WHITE, C2D_AlignLeft);
			text("3D", x + w1, 68, big, C_CYAN, C2D_AlignLeft);
		}
		const char* hint = app.busy                 ? app.busy_text
		                   : app.tab == TAB_HISTORY ? "Videos you watch will appear here"
		                   : app.tab == TAB_FILES   ? "Put videos (MKV, MP4) in /videos on the SD card"
		                   : app.tab == TAB_PICKS   ? "Tap 3D picks again to load"
		                                            : "Tap Search or press Y";
		text(hint, TOP_CX + DX(1), 136, 0.5f, C_WHITE, C2D_AlignCenter);
		if (osGet3DSliderState() <= 0.0f)
			text("Raise the 3D slider to see depth", TOP_CX, 205, 0.45f, C_AMBER, C2D_AlignCenter);
	}
	else
	{
		const int sel = SELECTED(app.tab, n);
		carousel(list, n, app.tab);
		info_strip(&list[sel], sel, n);
	}

	if (app.busy && n > 0 && !app.loading_more)
	{
		C2D_DrawRectSolid(0, 0, 0.5f, TOP_W, TOP_H, RGBA(0, 0, 0, 140));
		spinner(TOP_CX + DX(3), 100, 16, C_WHITE);
		text(app.busy_text, TOP_CX + DX(2), 128, 0.55f, C_WHITE, C2D_AlignCenter);
	}
}

// Big "now playing" card, used while a video loads or when a message is shown.
static void top_now_playing(bool loading)
{
	background();
	header(true);
	const VideoEntry* e = app.now_playing;
	float w = THUMB_W, h = THUMB_H, x = 88 + DX(3.5f), y = 30;   // the thumbnail's own size: 1:1, sharp
	rrect(x - 3, y - 3, w + 6, h + 6, 9, C_WHITE);
	C2D_Image l, r;
	int disp = 0;
	if (e && thumbs_get(e->id, app_layout(e), &l, &r, NULL, &disp))
		thumb_eye(g_eye < 0 ? r : l, x, y, w, h, disp, NULL);
	else rrect(x, y, w, h, 6, e ? hash_color(e->id) : C_PANEL);
	if (loading)
	{
		C2D_DrawCircleSolid(x + w / 2, y + h / 2, 0.5f, 30, RGBA(0, 0, 0, 150));
		spinner(x + w / 2 + DX(1.0f), y + h / 2, 17, C_WHITE);
	}
	if (e)
	{
		char title[128];
		clip(title, sizeof(title), e->title, 90);
		text_wrap(title, TOP_CX + DX(1.5f), 176, 0.58f, C_WHITE, C2D_AlignCenter, 370);
	}
}

// Pixels of shift per depth unit and eye at full slider, for each "Menu 3D" setting. The
// selected card sits at depth 4, so Normal gives it 4 * 0.6 * 2 = 4.8 px of total parallax.
// Keep it modest: around 14 px is more than many people can fuse.
static const float menu_depth_factors[4] = { 0.0f, 0.35f, 0.6f, 0.9f };
static const char* menu_depth_names[4] = { "Off", "Low", "Normal", "Strong" };

static float menu_depth_factor(void)
{
	int setting = app.menu_depth;
	return menu_depth_factors[setting < 0 || setting > 3 ? 2 : setting];
}

// Renders the top screen once, or once per eye when the 3D slider is up.
static void draw_top(void (*scene)(bool), bool arg, void (*scene0)(void))
{
	float slider = osGet3DSliderState();
	g_depth_px = slider * menu_depth_factor();
	C2D_TargetClear(top_l, C_BG);
	begin_scene(top_l);
	g_eye = slider > 0.0f ? 1.0f : 0.0f;
	if (scene) scene(arg);
	else scene0();
	if (slider > 0.0f)
	{
		C2D_TargetClear(top_r, C_BG);
		begin_scene(top_r);
		g_eye = -1.0f;
		if (scene) scene(arg);
		else scene0();
	}
	g_eye = 0.0f;
}

static void update_bg(void)
{
	// fade the background towards the selected video's average colour
	float want_r = 11, want_g = 13, want_b = 18;
	int n;
	VideoEntry* list = app_list(app.tab, &n);
	const VideoEntry* e = app.now_playing;
	if (!e && n > 0 && app.tab != TAB_SETTINGS) e = &list[SELECTED(app.tab, n)];
	C2D_Image left_img, right_img;
	u32 avg;
	if (e && thumbs_get(e->id, app_layout(e), &left_img, &right_img, &avg, NULL))
	{
		want_r = ((avg >> 16) & 0xFF) * 0.35f;
		want_g = ((avg >> 8) & 0xFF) * 0.35f;
		want_b = (avg & 0xFF) * 0.35f + 6;
	}
	bg_r += (want_r - bg_r) * 0.08f;
	bg_g += (want_g - bg_g) * 0.08f;
	bg_b += (want_b - bg_b) * 0.08f;
}

// ---------------------------------------------------------------- bottom screen: browsing

// layout, shared by drawing and hit testing
#define PANEL_X      8   // a full-width panel or button: PANEL_W wide, 8 px from either edge
#define PANEL_W      304
#define PANEL_RIGHT  (PANEL_X + PANEL_W)
#define HALF_L_X     8   // two buttons side by side, each HALF_W wide
#define HALF_R_X     164
#define HALF_W       148
#define SEARCH_X     8
#define SEARCH_Y     8
#define SEARCH_W     304
#define SEARCH_BTN_W 60   // the Search button beside the bar, shown once there's a query
#define SEARCH_BTN_X (SEARCH_X + SEARCH_W - SEARCH_BTN_W)
#define SEARCH_H     32
#define PREV_X       8
#define NEXT_X       164
#define NAV_Y        50
#define NAV_W        148
#define NAV_H        50
#define PLAY_X       8
#define PLAY_Y       108
#define PLAY_W       304
#define PLAY_H       58
#define BIG_Y        12   // with nothing listed yet: a big search field with quick picks under it
#define BIG_H        60
#define QP_Y         98   // quick picks: a 3 x 2 grid of QP_W x QP_H buttons
#define QP_W         96
#define QP_H         40
#define QP_STEP_X    104
#define QP_STEP_Y    48
#define TABS_Y       200
// Settings, bottom screen: three pages (3D, Playback, More) picked by a switcher at the top
#define PAGES_Y      8   // the page switcher
#define PAGES_H      30
#define PAGES_W      (PANEL_W / 3)
#define DEPTH_Y      64   // 3D page: Menu 3D Off / Low / Normal / Strong
#define DEPTH_W      76
#define MODE_Y       122   // 3D page: the video layout buttons, 3 x 2 of MODE_W x MODE_H
#define MODE_W       96
#define MODE_H       28
#define MODE_STEP_X  104
#define MODE_STEP_Y  32
#define ROW_Y        50   // Playback and More pages: rows of ROW_H every ROW_STEP
#define ROW_H        42
#define ROW_STEP     48
#define SWITCH_W     44    // an on/off switch at the right of a row
#define MOTION_SEG_W 140   // the Motion row's Normal / Smooth choice at the right

static const char* tab_names[TAB_COUNT] = { "Home", "3D picks", "History", "Videos", "Settings" };
#define TAB_W (BOT_W / TAB_COUNT)
static const char* mode_short[MODE_COUNT] = { "Auto", "2D", "3D SBS", "3D TB", "SBS swap", "TB swap" };

// The search bar, which opens the keyboard when tapped. Once there's a query, a Search button
// beside it runs the same search again without the keyboard.
static float search_bar_w(void) { return app.query[0] ? SEARCH_W - SEARCH_BTN_W - 8 : SEARCH_W; }

static void search_bar(void)
{
	rrect(SEARCH_X, SEARCH_Y, search_bar_w(), SEARCH_H, 8, C_PANEL);
	C2D_DrawCircleSolid(24, 23, 0.5f, 6, C_MUTED);
	C2D_DrawCircleSolid(24, 23, 0.5f, 4, C_PANEL);
	C2D_DrawLine(28, 27, C_MUTED, 32, 31, C_MUTED, 2, 0.5f);
	char q[96];
	clip(q, sizeof(q), app.query[0] ? app.query : "Search YouTube", 32);
	text(q, 40, 14, 0.5f, app.query[0] ? C_WHITE : C_MUTED, C2D_AlignLeft);
	if (app.query[0])
	{
		rrect(SEARCH_BTN_X, SEARCH_Y, SEARCH_BTN_W, SEARCH_H, 8, C_RED);
		text("Search", SEARCH_BTN_X + SEARCH_BTN_W / 2, 14, 0.5f, C_WHITE, C2D_AlignCenter);
	}
}

static void tab_bar(void)
{
	C2D_DrawRectSolid(0, TABS_Y, 0.5f, BOT_W, BOT_H - TABS_Y, RGBA(24, 27, 33, 255));
	for (int i = 0; i < TAB_COUNT; i++)
	{
		bool on = app.tab == (Tab)i;
		text(tab_names[i], i * TAB_W + TAB_W / 2, TABS_Y + 11, 0.5f, on ? C_WHITE : C_MUTED, C2D_AlignCenter);
		if (on) C2D_DrawRectSolid(i * TAB_W + TAB_W / 2 - 18, TABS_Y + 34, 0.5f, 36, 3, C_RED);
	}
}

// Videos tab, bottom screen: a header with Up, the folder name and Open/Play, then the list.
// Tapping a row selects it, tapping it again opens or plays it, and the bar on the right pages.
#define FB_HEAD_H 30
#define FB_UP_W   58
#define FB_GO_W   78
#define FB_GO_X   (304 - FB_GO_W)   // right edge lines up with the list's rows
#define FB_MORE_W 34                // a saved place's ⋮ at the right end of its row
#define FB_ROW_Y  34
#define FB_ROW_H  32
#define FB_ROWS   5
static int g_files_first = 0;   // index of the first visible row, for ui_hit

int ui_files_first(void) { return g_files_first; }

static void small_icon(const VideoEntry* e, float x, float y)
{
	const int kind = place_kind(e);
	if (kind) place_logo(kind, x, y, 22);
	else if (e->folder)
	{
		rrect(x + 1, y + 3, 9, 5, 2, C_AMBER);
		rrect(x + 1, y + 6, 20, 14, 3, C_AMBER);
	}
	else if (e->other)
	{
		// a file that isn't a video: a plain page
		rrect(x + 4, y + 1, 14, 20, 2, C_PANEL2);
		C2D_DrawRectSolid(x + 7, y + 7, 0.5f, 8, 1.5f, C_MUTED);
		C2D_DrawRectSolid(x + 7, y + 11, 0.5f, 8, 1.5f, C_MUTED);
		C2D_DrawRectSolid(x + 7, y + 15, 0.5f, 5, 1.5f, C_MUTED);
	}
	else
	{
		rrect(x + 1, y + 4, 20, 15, 3, C_CYAN);
		tri_right(x + 8, y + 11.5f, 5, C_CYANDK);
	}
}

// Videos tab, the sources (SD card, SMB, SFTP): one big tile each across the screen; a tap opens one
#define SRC_Y   10
#define SRC_H   182
#define SRC_GAP 8
static float src_tile_w(int n) { return ((float)PANEL_W - (n - 1) * SRC_GAP) / n; }

static void bottom_sources(const VideoEntry* list, int n, int sel)
{
	const float w = src_tile_w(n);
	for (int i = 0; i < n; i++)
	{
		const VideoEntry* e = &list[i];
		const bool on = i == sel;
		const float x = PANEL_X + i * (w + SRC_GAP);
		rrect(x, SRC_Y, w, SRC_H, 12, on ? C_PANEL2 : C_PANEL);
		const bool sd = !strcmp(e->path, FILES_SD_ROOT), smb = !strcmp(e->path, "smb:");
		place_logo(place_kind(e), x + w / 2 - 28, SRC_Y + 40, 56);
		text(sd ? "SD card" : smb ? "SMB" : "SFTP", x + w / 2, SRC_Y + 112, 0.6f, C_WHITE, C2D_AlignCenter);
		if (on) rrect(x + w / 2 - 18, SRC_Y + SRC_H - 14, 36, 4, 2, C_CYAN);
	}
	if (app.status[0])
	{
		char info[160];
		clip(info, sizeof(info), app.status, 60);
		text(info, BOT_CX, SRC_Y + SRC_H - 2, 0.42f, C_AMBER, C2D_AlignCenter);
	}
}

// Saved SMB and SFTP places get a ⋮ menu (edit, remove) in their row
static bool row_has_menu(const VideoEntry* e)
{
	const bool servers = !strcmp(app.files_dir, "smb:") || !strcmp(app.files_dir, "sftp:");
	const int kind = place_kind(e);
	return servers && (kind == PLACE_SMB || kind == PLACE_SFTP);
}

static void bottom_files(void)
{
	C2D_DrawRectSolid(0, 0, 0.5f, BOT_W, BOT_H, C_BG2);
	int n;
	VideoEntry* list = app_list(TAB_FILES, &n);
	if (!list) n = 0;
	if (!app.files_dir[0] && n > 0)
	{
		g_files_first = 0;
		bottom_sources(list, n, SELECTED(TAB_FILES, n));
		tab_bar();
		return;
	}
	const int sel = n > 0 ? SELECTED(TAB_FILES, n) : 0;
	const VideoEntry* cur = n > 0 ? &list[sel] : NULL;

	// header: Up, where we are, Open or Play
	const bool can_up = app.files_dir[0] != 0;
	if (can_up)
	{
		rrect(4, 4, FB_UP_W, FB_HEAD_H - 6, 7, C_PANEL);
		tri_up(18, 4 + (FB_HEAD_H - 6) / 2.0f, 5, C_WHITE);
		text("Up", 28, 7, 0.5f, C_WHITE, C2D_AlignLeft);
	}
	char loc[300], shown[80];
	files_location(app.files_dir, loc, sizeof(loc));
	// just the last part of the path: "b" for "SD card > a > b", a plain name like "SMB shares" as is
	const char* last = loc;
	for (const char* p = strstr(loc, "\xE2\x80\xBA "); p; p = strstr(p + 1, "\xE2\x80\xBA ")) last = p + 4;
	clip(shown, sizeof(shown), last, 22);
	text(shown, can_up ? 70 : 10, 8, 0.5f, C_MUTED, C2D_AlignLeft);
	if (cur)
	{
		const char* go = cur->folder ? "Open" : "Play";
		const float go_x = FB_GO_X, go_w = text_w(go, 0.5f) + 14,
		            go_left = go_x + (FB_GO_W - go_w) / 2;                         // triangle and word centred together
		rrect(go_x, 4, FB_GO_W, FB_HEAD_H - 6, 7, cur->other ? C_PANEL : C_RED);   // grey: not a video
		tri_right(go_left + 3, 4 + (FB_HEAD_H - 6) / 2.0f, 5, C_WHITE);
		text(go, go_left + 14, 7, 0.5f, C_WHITE, C2D_AlignLeft);
	}

	// the list
	int first = sel - FB_ROWS / 2;
	if (first > n - FB_ROWS) first = n - FB_ROWS;
	if (first < 0) first = 0;
	g_files_first = first;
	if (n == 0)
		text(app.files_dir[0] ? "No videos in this folder" : "", BOT_CX, FB_ROW_Y + 60, 0.5f, C_MUTED, C2D_AlignCenter);
	for (int row = 0; row < FB_ROWS && first + row < n; row++)
	{
		const VideoEntry* e = &list[first + row];
		const bool on = first + row == sel;
		const float y = FB_ROW_Y + row * FB_ROW_H;
		rrect(4, y, 300, FB_ROW_H - 3, 7, on ? C_PANEL2 : C_PANEL);
		if (on) rrect(4, y + 5, 3, FB_ROW_H - 13, 1.5f, C_CYAN);
		small_icon(e, 12, y + 4);
		const bool has_menu = row_has_menu(e);
		if (has_menu)
		{
			// ⋮: edit or remove this place
			const float dots_x = 304 - FB_MORE_W / 2.0f, dots_y = y + (FB_ROW_H - 3) / 2.0f;
			for (int dot = -1; dot <= 1; dot++)
				C2D_DrawCircleSolid(dots_x, dots_y + dot * 6, 0.5f, 2, on ? C_WHITE : C_MUTED);
		}
		char name[120];
		clip(name, sizeof(name), e->title, e->folder ? 36 : 28);
		text(name, 42, y + 6, 0.5f, e->other ? C_MUTED : on ? C_WHITE : RGBA(205, 210, 220, 255), C2D_AlignLeft);
		if (!e->folder)
		{
			char size[24];
			snprintf(size, sizeof(size), "%.*s", size_len(e->channel), e->channel);
			text(size, 298, y + 7, 0.42f, C_MUTED, C2D_AlignRight);
		}
	}
	if (n > FB_ROWS)
	{
		// the page bar shows where we are in the list; tap its top or bottom half to page
		const float track_h = FB_ROWS * FB_ROW_H - 3;
		rrect(309, FB_ROW_Y, 7, track_h, 3.5f, C_PANEL);
		float thumb_h = track_h * FB_ROWS / n;
		if (thumb_h < 12) thumb_h = 12;
		rrect(309, FB_ROW_Y + (track_h - thumb_h) * (float)first / (float)(n - FB_ROWS), 7, thumb_h, 3.5f, C_MUTED);
	}
	if (app.status[0])
	{
		char info[160];
		clip(info, sizeof(info), app.status, 60);
		text(info, BOT_CX, FB_ROW_Y + FB_ROWS * FB_ROW_H - 2, 0.42f, C_AMBER, C2D_AlignCenter);
	}
	tab_bar();
}

// n labelled segments in one panel with the chosen one lit: cyan for a setting's value, a
// lighter panel for the page switcher.
static void segments(float x, float y, float w, float h, int n, const char* const* labels, int sel, bool page)
{
	rrect(x, y, w, h, 8, C_PANEL);
	float seg_w = w / n;
	for (int i = 0; i < n; i++)
	{
		bool on = i == sel;
		if (on) rrect(x + i * seg_w + 2, y + 2, seg_w - 4, h - 4, 6, page ? C_PANEL2 : C_CYAN);
		text(labels[i], x + i * seg_w + seg_w / 2, y + h / 2 - 9, 0.5f, on ? (page ? C_WHITE : C_CYANDK) : C_MUTED,
		     C2D_AlignCenter);
	}
}

static void on_off_switch(float x, float y, bool on)
{
	rrect(x, y, SWITCH_W, 24, 12, on ? C_CYAN : C_PANEL2);
	C2D_DrawCircleSolid(on ? x + SWITCH_W - 12 : x + 12, y + 12, 0.5f, 9, on ? C_WHITE : C_MUTED);
}

// a row of the Playback / More pages: a panel with its label on the left
static float settings_row(int i, const char* label)
{
	float y = ROW_Y + i * ROW_STEP;
	rrect(PANEL_X, y, PANEL_W, ROW_H, 8, C_PANEL);
	text(label, 20, y + 11, 0.55f, C_WHITE, C2D_AlignLeft);
	return y;
}

// The message line of the YouTube tabs. A pending message wins; otherwise, while the console is
// offline, it says how to get back online. It's computed from the live Wi-Fi state, so it
// survives tab changes and disappears as soon as the connection returns.
static const char* tab_status(void)
{
	if (app.status[0]) return app.status;
	if (!wifi_connected()) return "No internet connection. Settings > Wi-Fi test can connect.";
	return "";
}

static void bottom_settings(void)
{
	static const char* pages[3] = { "3D", "Playback", "More" };
	segments(PANEL_X, PAGES_Y, PANEL_W, PAGES_H, 3, pages, app.set_page, true);

	if (app.set_page == 0)
	{
		text("Menu 3D", 12, DEPTH_Y - 18, 0.45f, C_MUTED, C2D_AlignLeft);
		segments(PANEL_X, DEPTH_Y, PANEL_W, 30, 4, menu_depth_names, app.menu_depth & 3, false);
		text("Video 3D layout", 12, MODE_Y - 18, 0.45f, C_MUTED, C2D_AlignLeft);
		for (int m = 0; m < MODE_COUNT; m++)
		{
			int cx = PANEL_X + (m % 3) * MODE_STEP_X, cy = MODE_Y + (m / 3) * MODE_STEP_Y;
			bool on = app.mode == (PlayMode)m;
			rrect(cx, cy, MODE_W, MODE_H, 7, on ? C_CYAN : C_PANEL);
			text(mode_short[m], cx + MODE_W / 2, cy + 6, 0.47f, on ? C_CYANDK : C_WHITE, C2D_AlignCenter);
		}
	}
	else if (app.set_page == 1)
	{
		float y = settings_row(0, "Subtitles");
		on_off_switch(PANEL_RIGHT - 12 - SWITCH_W, y + 9, app.subtitles);
		y = settings_row(1, "Motion");
		static const char* motion_names[MOTION_COUNT] = { "Normal", "Smooth" };
		segments(PANEL_RIGHT - 6 - MOTION_SEG_W, y + 6, MOTION_SEG_W, 30, MOTION_COUNT, motion_names,
		         app.motion % MOTION_COUNT, false);
		y = settings_row(2, "Nerd stats");
		on_off_switch(PANEL_RIGHT - 12 - SWITCH_W, y + 9, app.nerd_stats);
	}
	else
	{
		float y = settings_row(0, "Wi-Fi test");
		text(wifi_connected() ? "Online" : "Offline", 282, y + 13, 0.45f, wifi_connected() ? C_CYAN : C_RED,
		     C2D_AlignRight);
		tri_right(294, y + ROW_H / 2, 6, C_MUTED);
		y = settings_row(1, "Version");   // the version and commit, e.g. "1.0.0 (e206264)"
		char build[40];
		clip(build, sizeof(build), app_build_id(), 32);
		text(build, 298, y + 13, 0.45f, C_MUTED, C2D_AlignRight);
	}
}

static void bottom_browse(void)
{
	if (app.tab == TAB_FILES)
	{
		bottom_files();
		return;
	}
	C2D_DrawRectSolid(0, 0, 0.5f, BOT_W, BOT_H, C_BG2);
	if (app.tab == TAB_SETTINGS)
	{
		bottom_settings();
		tab_bar();
		return;
	}
	int n;
	VideoEntry* list = app_list(app.tab, &n);
	if (n == 0)
	{
		// nothing to browse yet: just a big search field with a red edge that opens the keyboard
		rrect(PANEL_X, BIG_Y, PANEL_W, BIG_H, 16, C_RED);
		rrect(10, BIG_Y + 2, 300, BIG_H - 4, 14, C_PANEL);
		const float icon_y = BIG_Y + BIG_H / 2;
		C2D_DrawCircleSolid(36, icon_y - 3, 0.5f, 10, C_WHITE);   // a magnifier
		C2D_DrawCircleSolid(36, icon_y - 3, 0.5f, 7, C_PANEL);
		C2D_DrawLine(43, icon_y + 4, C_WHITE, 50, icon_y + 11, C_WHITE, 3.5f, 0.5f);
		char q[96];
		clip(q, sizeof(q), app.query[0] ? app.query : "Search YouTube", 40);
		text(q, 62, icon_y - 13, 0.75f, app.query[0] ? C_WHITE : C_MUTED, C2D_AlignLeft);
		// quick picks: a tap runs that search, and the 3D ones (with "3D" in cyan) run a 3D
		// search. A message such as "Searching..." or an error replaces their heading.
		char info[200];
		const char* status = tab_status();
		clip(info, sizeof(info), status[0] ? status : "Popular", 70);
		text(info, status[0] ? BOT_CX : 12, QP_Y - 20, 0.45f, status[0] ? C_AMBER : C_MUTED,
		     status[0] ? C2D_AlignCenter : C2D_AlignLeft);
		for (int i = 0; i < QUICK_PICKS; i++)
		{
			const QuickPick* p = &quick_picks[i];
			const float x = PANEL_X + (i % 3) * QP_STEP_X, y = QP_Y + (i / 3) * QP_STEP_Y;
			rrect(x, y, QP_W, QP_H, 10, C_PANEL);
			const float w_3d = p->three_d ? text_w("3D ", 0.5f) : 0, w_label = text_w(p->label, 0.5f);
			const float text_x = x + QP_W / 2 - (w_3d + w_label) / 2;
			if (p->three_d) text("3D ", text_x, y + 11, 0.5f, C_CYAN, C2D_AlignLeft);
			text(p->label, text_x + w_3d, y + 11, 0.5f, C_WHITE, C2D_AlignLeft);
		}
		tab_bar();
		return;
	}
	search_bar();
	rrect(PREV_X, NAV_Y, NAV_W, NAV_H, 10, C_PANEL);
	tri_left(PREV_X + 44, NAV_Y + NAV_H / 2, 8, C_WHITE);
	text("Prev", PREV_X + 58, NAV_Y + 15, 0.6f, C_WHITE, C2D_AlignLeft);
	rrect(NEXT_X, NAV_Y, NAV_W, NAV_H, 10, C_PANEL);
	text("Next", NEXT_X + 90, NAV_Y + 15, 0.6f, C_WHITE, C2D_AlignRight);
	tri_right(NEXT_X + 100, NAV_Y + NAV_H / 2, 8, C_WHITE);

	const VideoEntry* cur = &list[SELECTED(app.tab, n)];
	const bool is3d = strcmp(app_layout(cur), "2d") != 0, folder = cur->folder;
	const char* play_label = folder ? "Open" : is3d ? "Play in 3D" : "Play";
	rrect(PLAY_X, PLAY_Y, PLAY_W, PLAY_H, 12, C_RED);
	float label_w = text_w(play_label, 0.75f);
	float label_x = BOT_CX - (label_w + 26) / 2;
	tri_right(label_x, PLAY_Y + PLAY_H / 2, 9, C_WHITE);
	text(play_label, label_x + 26, PLAY_Y + 16, 0.75f, C_WHITE, C2D_AlignLeft);

	// a message, or else the 3D mode the selected video will play in (2D videos have none)
	char info[200] = "";
	if (tab_status()[0]) clip(info, sizeof(info), tab_status(), 70);
	else if (app.loading_more && app.sel[app.tab] >= n - 2) snprintf(info, sizeof(info), "Loading more videos...");
	else if (is3d && !folder) snprintf(info, sizeof(info), "Mode: %s", play_mode_name(app.mode));
	if (info[0]) text(info, BOT_CX, 176, 0.45f, C_MUTED, C2D_AlignCenter);
	tab_bar();
}

UiAction ui_hit(int x, int y)
{
	if (app.tab == TAB_FILES && y < TABS_Y)
	{
		if (!app.files_dir[0])
		{
			int n;
			app_list(TAB_FILES, &n);
			if (n <= 0 || y < SRC_Y || y >= SRC_Y + SRC_H) return UA_NONE;
			const float w = src_tile_w(n);
			const int i = (int)((x - PANEL_X) / (w + SRC_GAP));
			if (x < PANEL_X || i >= n || x > PANEL_X + i * (w + SRC_GAP) + w) return UA_NONE;
			return (UiAction)(UA_FILES_ROW_0 + i);
		}
		if (y < FB_HEAD_H)
		{
			if (x < FB_UP_W + 8) return UA_FILES_UP;
			if (x >= FB_GO_X) return UA_PLAY;
			return UA_NONE;
		}
		if (x >= 305 && y >= FB_ROW_Y) return y < FB_ROW_Y + FB_ROWS * FB_ROW_H / 2 ? UA_FILES_PGUP : UA_FILES_PGDN;
		int row = (y - FB_ROW_Y) / FB_ROW_H;
		if (y >= FB_ROW_Y && row < FB_ROWS)
		{
			int n;
			const VideoEntry* list = app_list(TAB_FILES, &n);
			const int i = g_files_first + row;
			if (list && i < n && x >= 304 - FB_MORE_W && row_has_menu(&list[i]))
				return (UiAction)(UA_FILES_MORE_0 + row);
			return (UiAction)(UA_FILES_ROW_0 + row);
		}
		return UA_NONE;
	}
	if (y >= TABS_Y) return (UiAction)(UA_TAB_HOME + (x / TAB_W < TAB_COUNT ? x / TAB_W : TAB_COUNT - 1));
	if (app.tab == TAB_SETTINGS)
	{
		if (in_rect(x, y, PANEL_X, PAGES_Y, PANEL_W, PAGES_H + 4))
			return (UiAction)(UA_SET_PAGE_0 + ((x - PANEL_X) / PAGES_W < 2 ? (x - PANEL_X) / PAGES_W : 2));
		if (app.set_page == 0)
		{
			if (in_rect(x, y, PANEL_X, DEPTH_Y - 4, PANEL_W, 38))
				return (UiAction)(UA_DEPTH_0 + ((x - PANEL_X) / DEPTH_W < 3 ? (x - PANEL_X) / DEPTH_W : 3));
			for (int m = 0; m < MODE_COUNT; m++)
				if (in_rect(x, y, PANEL_X + (m % 3) * MODE_STEP_X, MODE_Y + (m / 3) * MODE_STEP_Y, MODE_W, MODE_H))
					return (UiAction)(UA_MODE_0 + m);
			return UA_NONE;
		}
		int row = (y - ROW_Y) / ROW_STEP;   // the whole row is the target
		if (y < ROW_Y || row > 2 || y >= ROW_Y + row * ROW_STEP + ROW_H) return UA_NONE;
		if (app.set_page == 2) return row == 0 ? UA_WIFI : UA_NONE;
		if (row == 0) return UA_SUBTITLES;
		if (row == 2) return UA_NERD_STATS;
		// Motion: tapping the half that's already chosen does nothing, anywhere else toggles it
		const int seg_x = PANEL_RIGHT - 6 - MOTION_SEG_W;
		if (x >= seg_x && (x - seg_x) / (MOTION_SEG_W / 2) == app.motion % MOTION_COUNT) return UA_NONE;
		return UA_MOTION;
	}
	int n;
	app_list(app.tab, &n);
	if (n == 0)
	{
		if (in_rect(x, y, PANEL_X, BIG_Y, PANEL_W, BIG_H)) return UA_SEARCH;
		for (int i = 0; i < QUICK_PICKS; i++)
			if (in_rect(x, y, PANEL_X + (i % 3) * QP_STEP_X, QP_Y + (i / 3) * QP_STEP_Y, QP_W, QP_H))
				return (UiAction)(UA_PICK_0 + i);
		return UA_NONE;
	}
	if (app.query[0] && in_rect(x, y, SEARCH_BTN_X, SEARCH_Y, SEARCH_BTN_W, SEARCH_H)) return UA_SEARCH_AGAIN;
	if (in_rect(x, y, SEARCH_X, SEARCH_Y, search_bar_w(), SEARCH_H)) return UA_SEARCH;
	if (in_rect(x, y, PREV_X, NAV_Y, NAV_W, NAV_H)) return UA_PREV;
	if (in_rect(x, y, NEXT_X, NAV_Y, NAV_W, NAV_H)) return UA_NEXT;
	if (in_rect(x, y, PLAY_X, PLAY_Y, PLAY_W, PLAY_H)) return UA_PLAY;
	return UA_NONE;
}

// ---------------------------------------------------------------- public: browsing

// ---- render targets: 2x2 supersampled in the menus
// The GPU draws shapes without anti-aliasing, so arcs, circles and rounded corners come out
// stair-stepped on the 400x240 screen. In the menus we therefore draw into targets twice the
// screen's size and let the display transfer average each 2x2 block on its way to the screen
// (SCALE_XY mode, with equal input and output dimensions as the hardware wants). Drawing code
// still works in screen units because begin_scene sets the projection to the screen size.
//
// This costs about 5 of the 6 MB of VRAM: 1.5 MB per top eye and 1.2 MB for the bottom
// screen (RGBA8), plus one 750 KB depth buffer shared by all three. Sharing is safe because
// citro2d draws every shape at the same depth, so the depth contents never matter.
// During playback we switch to screen-size targets (ui_playback): the video textures need the
// VRAM, and video frames shouldn't pay for four times the pixels.
static bool g_supersampled = false;   // the current targets are supersampled
static bool g_playing = false;        // a video is on screen (between ui_playback(true) and (false))
// dragging the player's seek bar (see pui_touch)
static bool scrubbing = false;   // the stylus is on the seek bar
static float scrub_frac = 0;     // and points at this fraction of the video (0..1)
bool pui_scrubbing(void) { return scrubbing; }
static void* g_shared_depth = NULL;   // the shared depth buffer

static void destroy_targets(void)
{
	if (top_l) C3D_RenderTargetDelete(top_l);   // waits for the GPU to finish with them
	if (top_r) C3D_RenderTargetDelete(top_r);
	if (bot) C3D_RenderTargetDelete(bot);
	top_l = top_r = bot = NULL;
	if (g_shared_depth) vramFree(g_shared_depth);
	g_shared_depth = NULL;
	g_supersampled = false;
}

static C3D_RenderTarget* ss_target(int w, int h, gfxScreen_t screen, gfx3dSide_t side)
{
	C3D_RenderTarget* t = C3D_RenderTargetCreate(w, h, GPU_RB_RGBA8, -1);   // no depth of its own
	if (!t) return NULL;
	C3D_FrameBufDepth(&t->frameBuf, g_shared_depth, GPU_RB_DEPTH16);
	C3D_RenderTargetSetOutput(t, screen, side,
	                          GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
	                              GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
	                              GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
	                              GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY));
	return t;
}

static bool create_targets(bool supersample)
{
	destroy_targets();
	if (supersample)
	{
		// twice the screens' size each way, in their rotated layout (height x width)
		g_shared_depth = vramAlloc(2 * TOP_H * 2 * TOP_W * 2);   // 16-bit depth
		if (g_shared_depth)
		{
			top_l = ss_target(2 * TOP_H, 2 * TOP_W, GFX_TOP, GFX_LEFT);
			top_r = ss_target(2 * TOP_H, 2 * TOP_W, GFX_TOP, GFX_RIGHT);
			bot = ss_target(2 * BOT_H, 2 * BOT_W, GFX_BOTTOM, GFX_LEFT);
		}
		if (top_l && top_r && bot)
		{
			g_supersampled = true;
			return true;
		}
		destroy_targets();   // not enough VRAM, so fall back to screen-size targets
	}
	top_l = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	top_r = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
	bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	return top_l && top_r && bot;
}

// C2D_SceneBegin, but keeping the projection in screen units on a supersampled target too
static void begin_scene(C3D_RenderTarget* t)
{
	C2D_SceneBegin(t);
	if (g_supersampled) C2D_SceneSize(TOP_H, t == bot ? BOT_W : TOP_W, true);
}

void ui_playback(bool playing)
{
	g_playing = playing;
	scrubbing = false;   // a drag doesn't outlive its video
	if (playing == !g_supersampled) return;
	if (!create_targets(!playing)) create_targets(false);
}

bool ui_init(void)
{
	gfxSet3D(true);   // stereo output is used whenever the slider is up
	if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) return false;
	// The busiest menu frame (the loading screen in both eyes) draws about 600 shapes and
	// glyphs. A playback frame with nerd stats in both eyes, two subtitle lines and the comments
	// panel gets to about 1600. A limit of 2560 leaves a margin and still saves ~230 KB of
	// linear memory compared with the default of 4096.
	if (!C2D_Init(UI_MAX_OBJECTS)) return false;
	C2D_Prepare();
	bool ok = create_targets(true);
	frame_text = C2D_TextBufNew(UI_MAX_OBJECTS);   // glyphs are a subset of the objects above
	measure_text = C2D_TextBufNew(1024);
	uifont_init();   // without it every label falls back to the system font
	logo_init();     // without it the logo is drawn as text
	vmesh_init();    // without it Motion: Smooth shows plain frames
	return ok && frame_text && measure_text;
}

bool ui_supersampled(void) { return g_supersampled; }
bool ui_font_ok(void) { return uifont_ready(); }

void ui_exit(void)
{
	uifont_exit();
	logo_exit();
	vmesh_exit();
	destroy_targets();
	if (frame_text) C2D_TextBufDelete(frame_text);
	C2D_Fini();
	C3D_Fini();
}

// Once the system is closing the app (Close from the HOME menu), the GPU is no longer ours and
// citro3d's refresh counting is off, so C3D_FrameBegin would wait forever. On hardware that
// left the console stuck on "Closing software..." when a video was closed this way. Every
// drawing entry point checks this first.
static bool closing(void) { return aptShouldClose(); }

// Starts a menu frame: the browsing scene on the top screen, the bottom scene begun and cleared
static void begin_menu_frame(void)
{
	app.frame++;
	update_bg();
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C2D_TextBufClear(frame_text);
	draw_top(NULL, false, top_browse);
	C2D_TargetClear(bot, C_BG2);
	begin_scene(bot);
}

void ui_draw(void)
{
	if (closing()) return;
	begin_menu_frame();
	bottom_browse();
	C3D_FrameEnd(0);
}

// ---------------------------------------------------------------- the Wi-Fi test (Settings)

#define WIFI_ROW_Y 44
#define WIFI_BTN_Y 190
#define WIFI_BTN_H 40

static void wifi_row(int i, const char* label, WifiStep step, const char* detail, bool busy)
{
	float y = WIFI_ROW_Y + i * 30;
	float dot_y = y + 8;
	if (step == WT_PENDING && busy) spinner(20, dot_y, 6, C_CYAN);
	else
	{
		u32 c = step == WT_OK ? RGBA(46, 213, 115, 255) : step == WT_FAIL ? C_RED : C_PANEL2;
		C2D_DrawCircleSolid(20, dot_y, 0.5f, 6, c);
	}
	text(label, 34, y, 0.55f, C_WHITE, C2D_AlignLeft);
	char shown[96];
	clip(shown, sizeof(shown), detail, 34);
	text(shown, 308, y + 1, 0.47f, C_MUTED, C2D_AlignRight);
}

void ui_wifi(const WifiTest* t, bool connecting)
{
	if (closing()) return;
	begin_menu_frame();
	bool busy = !t->done || connecting;
	text("Connection test", BOT_CX, 10, 0.65f, C_WHITE, C2D_AlignCenter);

	char detail[96];
	if (t->link == WT_OK)
	{
		if (t->ssid[0]) snprintf(detail, sizeof(detail), "%s  \xC2\xB7  signal %u/3", t->ssid, t->bars);
		else snprintf(detail, sizeof(detail), "signal %u/3", t->bars);
	}
	else snprintf(detail, sizeof(detail), t->link == WT_FAIL ? "not connected" : "checking...");
	wifi_row(0, "Wi-Fi", t->link, detail, busy);

	const WifiStep steps[2] = { t->internet, t->youtube };
	const u32 ms[2] = { t->internet_ms, t->youtube_ms };
	const char* labels[2] = { "Internet", "YouTube" };
	for (int i = 0; i < 2; i++)
	{
		if (steps[i] == WT_OK) snprintf(detail, sizeof(detail), "%lu ms", (unsigned long)ms[i]);
		else if (steps[i] == WT_FAIL) snprintf(detail, sizeof(detail), "no answer");
		else if (steps[i] == WT_SKIP) snprintf(detail, sizeof(detail), "skipped");
		else snprintf(detail, sizeof(detail), "%s", busy ? "checking..." : "");
		wifi_row(i + 1, labels[i], steps[i], detail, busy);
	}

	// one line saying what the results mean
	const char* hint;
	if (connecting) hint = "Connecting with your saved Wi-Fi networks...";
	else if (!t->done) hint = "Testing...";
	else if (t->link != WT_OK) hint = "Not online. Connect uses the networks saved in System Settings.";
	else if (t->internet != WT_OK) hint = "On Wi-Fi but no internet. Check the router.";
	else if (t->youtube != WT_OK) hint = "The internet works but YouTube doesn't answer.";
	else hint = "Everything works.";
	text_wrap(hint, BOT_CX, 158, 0.45f,
	          t->done && !connecting && t->link == WT_OK && t->internet == WT_OK ? C_CYAN : C_MUTED, C2D_AlignCenter,
	          300);

	rrect(HALF_L_X, WIFI_BTN_Y, HALF_W, WIFI_BTN_H, 10, C_PANEL);
	text("Back", HALF_L_X + HALF_W / 2, WIFI_BTN_Y + 10, 0.6f, busy ? C_MUTED : C_WHITE, C2D_AlignCenter);
	rrect(HALF_R_X, WIFI_BTN_Y, HALF_W, WIFI_BTN_H, 10, busy ? C_PANEL : C_RED);
	text(t->done && t->link != WT_OK ? "Connect" : "Test again", HALF_R_X + HALF_W / 2, WIFI_BTN_Y + 10, 0.6f,
	     busy ? C_MUTED : C_WHITE, C2D_AlignCenter);
	C3D_FrameEnd(0);
}

// ---------------------------------------------------------------- a form page (network places)
#define FORM_ROW_Y 30
#define FORM_ROW_H 24
#define FORM_STEP  26
#define FORM_BTN_Y 194
#define FORM_BTN_H 38

void ui_form(const char* title, const UiField* f, int n, int sel, bool can_save)
{
	if (closing()) return;
	begin_menu_frame();
	text(title, BOT_CX, 5, 0.6f, C_WHITE, C2D_AlignCenter);
	for (int i = 0; i < n && i < UI_FORM_MAX; i++)
	{
		const float y = FORM_ROW_Y + i * FORM_STEP;
		const bool on = i == sel;
		rrect(PANEL_X, y, PANEL_W, FORM_ROW_H, 6, on ? C_PANEL2 : C_PANEL);
		if (on) rrect(PANEL_X, y + 4, 3, FORM_ROW_H - 8, 1.5f, C_CYAN);
		text(f[i].label, 18, y + 4, 0.45f, C_MUTED, C2D_AlignLeft);
		char shown[96];
		const bool empty = !f[i].value || !f[i].value[0];
		if (empty) clip(shown, sizeof(shown), f[i].placeholder ? f[i].placeholder : "", 40);
		else if (f[i].secret)
		{
			size_t stars = strlen(f[i].value);
			if (stars > 16) stars = 16;
			for (size_t j = 0; j < stars; j++) shown[j] = '*';
			shown[stars] = 0;
		}
		else clip(shown, sizeof(shown), f[i].value, 34);
		text(shown, 302, y + 4, 0.47f, empty ? RGBA(110, 118, 130, 255) : C_WHITE, C2D_AlignRight);
	}
	rrect(HALF_L_X, FORM_BTN_Y, HALF_W, FORM_BTN_H, 10, C_PANEL);
	text("Cancel", HALF_L_X + HALF_W / 2, FORM_BTN_Y + 9, 0.6f, C_WHITE, C2D_AlignCenter);
	rrect(HALF_R_X, FORM_BTN_Y, HALF_W, FORM_BTN_H, 10, can_save ? C_RED : C_PANEL);
	text("Save", HALF_R_X + HALF_W / 2, FORM_BTN_Y + 9, 0.6f, can_save ? C_WHITE : C_MUTED, C2D_AlignCenter);
	C3D_FrameEnd(0);
}

int ui_form_hit(int x, int y, int n)
{
	if (in_rect(x, y, HALF_L_X, FORM_BTN_Y, HALF_W, FORM_BTN_H)) return 1;
	if (in_rect(x, y, HALF_R_X, FORM_BTN_Y, HALF_W, FORM_BTN_H)) return 2;
	for (int i = 0; i < n && i < UI_FORM_MAX; i++)
		if (in_rect(x, y, PANEL_X, FORM_ROW_Y + i * FORM_STEP, PANEL_W, FORM_ROW_H)) return 100 + i;
	return 0;
}

#define MENU_ROW_Y 70
#define MENU_ROW_H 40
#define MENU_STEP  48
#define MENU_MAX   3   // choices that fit above Cancel

void ui_menu(const char* title, const char* sub, const char* const* items, int n, int sel, int danger)
{
	if (closing()) return;
	begin_menu_frame();
	char title_shown[64], sub_shown[96];
	clip(title_shown, sizeof(title_shown), title, 40);
	text(title_shown, BOT_CX, 12, 0.65f, C_WHITE, C2D_AlignCenter);
	clip(sub_shown, sizeof(sub_shown), sub ? sub : "", 60);
	text(sub_shown, BOT_CX, 38, 0.45f, C_MUTED, C2D_AlignCenter);
	for (int i = 0; i < n && i < MENU_MAX; i++)
	{
		const float y = MENU_ROW_Y + i * MENU_STEP;
		rrect(PANEL_X, y, PANEL_W, MENU_ROW_H, 10, i == sel ? C_PANEL2 : C_PANEL);
		if (i == sel) rrect(PANEL_X, y + 8, 3, MENU_ROW_H - 16, 1.5f, C_CYAN);
		text(items[i], BOT_CX, y + 10, 0.6f, i == danger ? C_RED : C_WHITE, C2D_AlignCenter);
	}
	rrect(PANEL_X, FORM_BTN_Y, PANEL_W, FORM_BTN_H, 10, C_PANEL);
	text("Cancel", BOT_CX, FORM_BTN_Y + 9, 0.6f, C_WHITE, C2D_AlignCenter);
	C3D_FrameEnd(0);
}

int ui_menu_hit(int x, int y, int n)
{
	if (in_rect(x, y, PANEL_X, FORM_BTN_Y, PANEL_W, FORM_BTN_H)) return 1;
	for (int i = 0; i < n && i < MENU_MAX; i++)
		if (in_rect(x, y, PANEL_X, MENU_ROW_Y + i * MENU_STEP, PANEL_W, MENU_ROW_H)) return 100 + i;
	return 0;
}

static bool touched(touchPosition* tp)
{
	if (!(hidKeysDown() & KEY_TOUCH)) return false;
	hidTouchRead(tp);
	return true;
}

// the Cancel button under a spinner (connecting, loading a video)
#define CANCEL_X 80
#define CANCEL_Y 184
#define CANCEL_W 160
#define CANCEL_H 40

static void cancel_button(void)
{
	rrect(CANCEL_X, CANCEL_Y, CANCEL_W, CANCEL_H, 10, C_PANEL);
	text("Cancel", CANCEL_X + CANCEL_W / 2, CANCEL_Y + 10, 0.6f, C_WHITE, C2D_AlignCenter);
}

static bool cancel_tapped(void)
{
	touchPosition tp;
	return touched(&tp) && in_rect(tp.px, tp.py, CANCEL_X, CANCEL_Y, CANCEL_W, CANCEL_H);
}

bool ui_connecting(const char* where)
{
	if (closing()) return true;
	begin_menu_frame();   // the top screen keeps the Videos tab, with the place that was picked
	text("Connecting", BOT_CX, 60, 0.7f, C_WHITE, C2D_AlignCenter);
	char where_shown[120];
	clip(where_shown, sizeof(where_shown), where ? where : "", 60);
	text_wrap(where_shown, BOT_CX, 92, 0.5f, C_MUTED, C2D_AlignCenter, 280);
	spinner(BOT_CX, 140, 16, C_CYAN);
	cancel_button();
	C3D_FrameEnd(0);
	return cancel_tapped();
}

int ui_wifi_hit(int x, int y)
{
	if (in_rect(x, y, HALF_L_X, WIFI_BTN_Y, HALF_W, WIFI_BTN_H)) return 1;
	if (in_rect(x, y, HALF_R_X, WIFI_BTN_Y, HALF_W, WIFI_BTN_H)) return 2;
	return 0;
}

void ui_fatal(const char* title, const char* msg)
{
	if (closing()) return;
	while (aptMainLoop())
	{
		hidScanInput();
		if (hidKeysDown() & KEY_START) break;
		app.frame++;
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TextBufClear(frame_text);
		C2D_TargetClear(top_l, C_BG);
		begin_scene(top_l);
		text(title, TOP_CX, 70, 0.8f, C_RED, C2D_AlignCenter);
		text_wrap(msg, 20, 110, 0.55f, C_WHITE, C2D_AlignCenter, 360);
		C2D_TargetClear(bot, C_BG2);
		begin_scene(bot);
		text("Press START to exit", BOT_CX, 110, 0.55f, C_MUTED, C2D_AlignCenter);
		C3D_FrameEnd(0);
	}
}

// ---------------------------------------------------------------- player UI (playerui.h)

static bool top_is_ours(void)
{
	// the menus draw the top screen only while it has citro2d's BGR8 format
	return gfxGetScreenFormat(GFX_TOP) == GSP_BGR8_OES;
}

bool pui_loading(const char* line1, const char* line2, int tick)
{
	if (closing()) return true;   // act as if Cancel was tapped, so the caller stops
	power_bottom_backlight(true);
	app.frame++;
	update_bg();
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C2D_TextBufClear(frame_text);
	if (top_is_ours()) draw_top(top_now_playing, true, NULL);
	C2D_TargetClear(bot, C_BG2);
	begin_scene(bot);
	// a second line is optional: without one the title sits a little lower, nearer the spinner
	const bool has_line2 = line2 && line2[0];
	text(line1, BOT_CX, has_line2 ? 52 : 68, 0.7f, C_WHITE, C2D_AlignCenter);
	if (has_line2) text_wrap(line2, BOT_CX, 84, 0.5f, C_MUTED, C2D_AlignCenter, 280);
	spinner(BOT_CX, 140, 16, C_CYAN);
	cancel_button();
	C3D_FrameEnd(0);
	return cancel_tapped();
}

// playback remote layout
#define META_Y       52   // channel and views, under the title
#define ACTION_Y     70   // YouTube only: like, dislike and Comments buttons, ACTION_H tall
#define ACTION_H     22
#define LIKE_X       16
#define DISLIKE_X    106
#define VOTE_W       84
#define COMMENTS_X   196
#define COMMENTS_W   108
#define HINT_Y       95
#define VOTE_NOTE_MS 2500   // how long the "Voting needs a YouTube account" note stays
#define BAR_X        16
#define BAR_Y        120
#define BAR_H        10
#define BAR_W        288
#define REWIND_X     16   // the -10 s and +10 s buttons, SEEK_BTN_W x SEEK_BTN_H
#define FORWARD_X    224
#define SEEK_BTN_W   80
#define SEEK_BTN_H   36
#define PAUSE_X      112   // the play / pause button between them, a little bigger
#define PAUSE_Y      (BTN_Y - 3)
#define PAUSE_W      96
#define PAUSE_H      42
#define BTN_Y        158
#define BACK_Y       204   // the bottom row: Back and Settings, BACK_H tall
#define BACK_H       28
#define BACK_X       16
#define BACK_W       140
#define SETTINGS_X   164
#define SETTINGS_W   140

// The playback settings page, which replaces the remote while it's open
#define SP_CLOSE_X   236   // Close, top right, SP_CLOSE_W x SP_CLOSE_H
#define SP_CLOSE_W   68
#define SP_CLOSE_H   26
#define SP_ROW_Y     40   // the first row
#define SP_ROW_H     30
#define SP_ROW_STEP  36
#define SP_DEPTH_H   56   // the depth label and its slider
#define SP_SLIDER_X0 30   // the slider's first and last dot
#define SP_SLIDER_X1 290
#define SP_TILE_H    36   // the Smooth motion and Nerd stats tiles, side by side
#define SP_MAX_ROWS  6

static void status_bottom(const PlayerStatus* st);
// What the last remote drawn looked like, for hit testing in pui_touch:
static bool status_set_open = false;   // it was the settings page
// the settings page's rows: where each is and what tapping it does (PUI_SET_*)
static struct {
	float y, h;
	int x, w, what;
} set_rows[SP_MAX_ROWS];
static int set_row_count = 0;
static float set_slider_y = -1;     // the depth slider's line, or -1 when not shown
static bool set_dragging = false;   // the stylus is on the depth slider
static int set_tapped = PUI_SET_NONE;
static bool status_actions = false;   // it had the like, dislike and Comments buttons
static u64 vote_note_until = 0;       // until then the hint line says why tapping like did nothing
static bool status_live = false;      // a live stream: no seek buttons

void pui_status(const PlayerStatus* st)
{
	if (closing()) return;
	if (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) return;   // GPU still busy with the last one; skip this update
	C2D_TextBufClear(frame_text);
	C2D_TargetClear(bot, C_BG2);
	begin_scene(bot);
	status_bottom(st);
	C3D_FrameEnd(0);
}

// The subtitle language list, which replaces the remote while it's open
#define CC_LIST_Y     30
#define CC_LIST_ROW_H 32
// Both lists end in a bar of three buttons: Up, Close and Down
#define LIST_BAR_Y   196
#define LIST_BAR_H   36
#define LIST_UP_X    12
#define LIST_DOWN_X  244
#define LIST_ARROW_W 64
#define LIST_CLOSE_X 84
#define LIST_CLOSE_W 152
static bool status_cc_menu = false;   // the last bottom screen drawn was this list
static int status_cc_top = 0, status_cc_rows = 0;
static int cc_tapped = -1;

// Draws that bar; an arrow is lit while there's more to scroll to that way
static void list_bar(bool more_up, bool more_down)
{
	rrect(LIST_UP_X, LIST_BAR_Y, LIST_ARROW_W, LIST_BAR_H, 8, C_PANEL);
	tri_up(LIST_UP_X + LIST_ARROW_W / 2, LIST_BAR_Y + LIST_BAR_H / 2, 8, more_up ? C_WHITE : C_PANEL2);
	rrect(LIST_CLOSE_X, LIST_BAR_Y, LIST_CLOSE_W, LIST_BAR_H, 8, C_PANEL);
	text("Close", LIST_CLOSE_X + LIST_CLOSE_W / 2, LIST_BAR_Y + 8, 0.55f, C_WHITE, C2D_AlignCenter);
	rrect(LIST_DOWN_X, LIST_BAR_Y, LIST_ARROW_W, LIST_BAR_H, 8, C_PANEL);
	tri_down(LIST_DOWN_X + LIST_ARROW_W / 2, LIST_BAR_Y + LIST_BAR_H / 2, 8, more_down ? C_WHITE : C_PANEL2);
}

static void cc_list(const PlayerStatus* st)
{
	const bool audio = st->audio_count > 0;   // a file's audio tracks follow the subtitles
	text(audio ? "Subtitles & audio" : "Subtitles", 16, 6, 0.55f, C_WHITE, C2D_AlignLeft);
	text("Up/Down + A, or tap", 304, 9, 0.42f, C_MUTED, C2D_AlignRight);
	int rows = st->cc_count + 1 + st->audio_count;
	status_cc_rows = rows;
	for (int r = st->cc_top; r < rows && r < st->cc_top + PUI_CC_ROWS; r++)
	{
		float y = CC_LIST_Y + (r - st->cc_top) * CC_LIST_ROW_H;
		const bool is_audio = r > st->cc_count;
		bool highlighted = r == st->cc_hl, chosen = is_audio ? r - st->cc_count - 1 == st->audio_sel : r == st->cc_sel;
		rrect(12, y, 296, CC_LIST_ROW_H - 4, 7, highlighted ? C_PANEL2 : C_PANEL);
		if (highlighted) rrect(12, y + 4, 4, CC_LIST_ROW_H - 12, 2, C_CYAN);   // the D-pad cursor
		if (chosen) C2D_DrawCircleSolid(28, y + (CC_LIST_ROW_H - 4) / 2.0f, 0.5f, 5, is_audio ? C_AMBER : C_CYAN);
		char name[112];
		if (is_audio)
		{
			char label[104];
			snprintf(label, sizeof(label), "Audio: %s", st->audio_names[r - st->cc_count - 1]);
			clip(name, sizeof(name), label, 60);
		}
		else clip(name, sizeof(name), r == 0 ? (audio ? "Subtitles off" : "Off") : st->cc_names[r - 1], 60);
		text(name, 42, y + 5, 0.5f, chosen ? (is_audio ? C_AMBER : C_CYAN) : C_WHITE, C2D_AlignLeft);
	}
	list_bar(st->cc_top > 0, st->cc_top + PUI_CC_ROWS < rows);
}

// ---- the comments panel, which replaces the remote while it's open. Each comment is a card
// with the author, age, text and likes; the list scrolls by dragging or with Up/Down. Each
// comment's line breaks are worked out only once.
#define CM_TOP    30   // the list sits between the header and the button bar
#define CM_BOTTOM 194
#define CM_TEXT_W 280
#define CM_LINES  16   // longer comments are cut here and end in "..."
#define CM_LINE_H 15
#define CM_MAX    128
typedef struct {
	u16 start[CM_LINES], len[CM_LINES];
	u8 lines;
	bool cut;   // more text than CM_LINES lines
	float h;    // the card's height with its gap
} CmLayout;
static CmLayout cm_layouts[CM_MAX];
static const YtComment* cm_layouts_of = NULL;   // the comment array cm_layouts belongs to
static int cm_laid_out = 0;                     // how many of its comments are laid out so far
static float cm_scroll = 0, cm_total = 0;       // pixels scrolled, and the height of all cards
static bool cm_drag = false;                    // the stylus is dragging the list
static int cm_drag_y = 0;
static float cm_drag_from = 0;
static bool status_cm_open = false;
static bool cm_moving = false;   // scrolled since the last frame

void pui_comments_scroll(float dy)
{
	cm_scroll += dy;
	cm_moving = true;
}
static bool cm_layout_pending = false;   // some comments aren't laid out yet, so keep drawing frames
bool pui_comments_active(void) { return cm_drag || cm_moving || cm_layout_pending; }
bool pui_comments_want_more(void) { return cm_total - cm_scroll < (CM_BOTTOM - CM_TOP) * 3; }
void pui_comments_reset(void)
{
	cm_scroll = cm_total = 0;
	cm_drag = cm_moving = false;
	cm_layouts_of = NULL;
	cm_laid_out = 0;
}

// Breaks one comment into lines of at most CM_TEXT_W pixels at scale 0.45, at spaces and at
// the comment's own line breaks.
static void cm_layout_comment(const char* s, CmLayout* lay)
{
	lay->lines = 0;
	lay->cut = false;
	const size_t n = strlen(s);
	char buf[YT_COMMENT_TEXT];
	const float space = text_w(" ", 0.45f);
	size_t i = 0;
	while (i < n && lay->lines < CM_LINES)
	{
		// build one line from i, adding words (each measured once) while they still fit
		const size_t line_start = i;
		size_t end = i;   // the line's end so far
		float line_w = 0;
		bool first = true;
		for (;;)
		{
			size_t word_start = end;
			while (word_start < n && s[word_start] == ' ') word_start++;
			if (word_start >= n || s[word_start] == '\n')
			{
				end = word_start;
				break;
			}
			size_t word_end = word_start;
			while (word_end < n && s[word_end] != ' ' && s[word_end] != '\n') word_end++;
			memcpy(buf, s + word_start, word_end - word_start);
			buf[word_end - word_start] = 0;
			const float word_w = text_w(buf, 0.45f);
			const float add = (first ? 0 : space) + word_w;
			if (line_w + add <= CM_TEXT_W)
			{
				line_w += add;
				end = word_end;
				first = false;
				continue;
			}
			if (!first) break;   // the word starts the next line
			// a single word wider than a whole line: cut it between characters
			size_t cut = word_start;
			float cut_w = 0;
			while (cut < word_end)
			{
				size_t next = cut + 1;
				while (next < word_end && ((unsigned char)s[next] & 0xC0) == 0x80) next++;
				memcpy(buf, s + cut, next - cut);
				buf[next - cut] = 0;
				const float char_w = text_w(buf, 0.45f);
				if (cut_w + char_w > CM_TEXT_W && cut > word_start) break;
				cut_w += char_w;
				cut = next;
			}
			end = cut;
			break;
		}
		lay->start[lay->lines] = (u16)line_start;
		lay->len[lay->lines] = (u16)(end - line_start);
		lay->lines++;
		i = end;
		if (i < n && s[i] == '\n') i++;
		else
			while (i < n && s[i] == ' ') i++;
		if (end == line_start && i == line_start) i++;   // always make progress
	}
	if (i < n) lay->cut = true;
	// a line can start with the spaces that separated it from the previous one; skip them
	for (int k = 0; k < lay->lines; k++)
		while (lay->len[k] && s[lay->start[k]] == ' ')
		{
			lay->start[k]++;
			lay->len[k]--;
		}
	lay->h = 24 + (lay->lines ? lay->lines : 1) * CM_LINE_H + 22;
}

static void comments_panel(const PlayerStatus* st)
{
	// lay out comments we haven't seen yet (a new page is appended to the array), a few per
	// frame so a big page doesn't stall the remote
	if (cm_layouts_of != st->cm || st->cm_count < cm_laid_out)
	{
		cm_layouts_of = st->cm;
		cm_laid_out = 0;
		cm_total = 0;
	}
	for (int budget = 4; cm_laid_out < st->cm_count && cm_laid_out < CM_MAX && budget > 0; cm_laid_out++, budget--)
	{
		cm_layout_comment(st->cm[cm_laid_out].text, &cm_layouts[cm_laid_out]);
		cm_total += cm_layouts[cm_laid_out].h;
	}
	cm_layout_pending = cm_laid_out < st->cm_count && cm_laid_out < CM_MAX;
	const float view_h = CM_BOTTOM - CM_TOP;
	const float foot_h = st->cm_loading || st->cm_error || !st->cm_count ? 40 : 8;   // room for a spinner or a message
	float max_scroll = cm_total + foot_h - view_h;
	if (max_scroll < 0) max_scroll = 0;
	if (cm_scroll > max_scroll) cm_scroll = max_scroll;
	if (cm_scroll < 0) cm_scroll = 0;
	cm_moving = false;

	float y = CM_TOP - cm_scroll;
	char line[YT_COMMENT_TEXT], when_text[96];
	for (int i = 0; i < cm_laid_out; i++)
	{
		const CmLayout* lay = &cm_layouts[i];
		const YtComment* com = &st->cm[i];
		if (y + lay->h < CM_TOP)
		{
			y += lay->h;
			continue;
		}
		if (y > CM_BOTTOM) break;
		rrect(8, y, 300, lay->h - 6, 8, C_PANEL);
		char who[56];
		clip(who, sizeof(who), com->author, 30);
		text(who, 18, y + 6, 0.45f, com->by_uploader ? C_CYAN : C_WHITE, C2D_AlignLeft);
		float when_x = 18 + text_w(who, 0.45f);
		snprintf(when_text, sizeof(when_text), "  \xC2\xB7  %s", com->when);
		text(when_text, when_x, y + 6, 0.42f, C_MUTED, C2D_AlignLeft);
		if (com->pinned) text("Pinned", 298, y + 6, 0.4f, C_AMBER, C2D_AlignRight);
		for (int k = 0; k < lay->lines; k++)
		{
			const float line_y = y + 24 + k * CM_LINE_H;
			if (line_y + CM_LINE_H < CM_TOP || line_y > CM_BOTTOM) continue;
			memcpy(line, com->text + lay->start[k], lay->len[k]);
			line[lay->len[k]] = 0;
			if (k == lay->lines - 1 && lay->cut) snprintf(line + lay->len[k], sizeof(line) - lay->len[k], "...");
			text(line, 18, line_y, 0.45f, RGBA(222, 226, 232, 255), C2D_AlignLeft);
		}
		// likes and replies
		char footer[48] = "";
		if (com->likes[0])
			snprintf(footer, sizeof(footer), "%s like%s", com->likes, strcmp(com->likes, "1") ? "s" : "");
		if (com->replies)
			snprintf(footer + strlen(footer), sizeof(footer) - strlen(footer), "%s%d repl%s",
			         footer[0] ? "  \xC2\xB7  " : "", com->replies, com->replies == 1 ? "y" : "ies");
		if (footer[0]) text(footer, 18, y + lay->h - 24, 0.4f, C_MUTED, C2D_AlignLeft);
		y += lay->h;
	}
	// at the end of the list: a spinner while a page loads, or why there are no comments
	if (y < CM_BOTTOM + 40)
	{
		if (st->cm_loading)
		{
			spinner(BOT_CX, y + 18, 9, C_MUTED);
			if (!st->cm_count) text("Loading comments", BOT_CX, y + 34, 0.45f, C_MUTED, C2D_AlignCenter);
		}
		else if (st->cm_error && !st->cm_count) text(st->cm_error, BOT_CX, y + 14, 0.45f, C_MUTED, C2D_AlignCenter);
		else if (!st->cm_count) text("No comments yet", BOT_CX, y + 14, 0.45f, C_MUTED, C2D_AlignCenter);
	}

	// the header and button bar are drawn last, over the list, so it scrolls under them
	C2D_DrawRectSolid(0, 0, 0.5f, BOT_W, CM_TOP - 2, C_BG2);
	text("Comments", 16, 6, 0.55f, C_WHITE, C2D_AlignLeft);
	if (st->cm_count)
	{
		char count_text[16];
		snprintf(count_text, sizeof(count_text), "%d", st->cm_count);
		text(count_text, 16 + text_w("Comments", 0.55f) + 8, 9, 0.42f, C_MUTED, C2D_AlignLeft);
	}
	text("Drag, or Up/Down", 304, 9, 0.42f, C_MUTED, C2D_AlignRight);
	C2D_DrawRectSolid(0, CM_BOTTOM, 0.5f, BOT_W, BOT_H - CM_BOTTOM, C_BG2);
	// a thin scroll bar on the right
	if (cm_total > view_h)
	{
		const float track_h = view_h - 8,
		            thumb_h = track_h * view_h / (cm_total + foot_h) < 16 ? 16 : track_h * view_h / (cm_total + foot_h);
		rrect(312, CM_TOP + 4 + (track_h - thumb_h) * (max_scroll > 0 ? cm_scroll / max_scroll : 0), 4, thumb_h, 2,
		      C_PANEL2);
	}
	list_bar(cm_scroll > 0, cm_scroll < max_scroll);
}

// Copies s into out, cut with "..." to fit width pixels at scale
static void fit_to_width(char* out, size_t cap, const char* s, float scale, float width)
{
	clip(out, cap, s, cap - 4);
	size_t n = strlen(s);
	while (text_w(out, scale) > width && n > 0)
	{
		n--;
		while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
		clip(out, cap, s, n);
	}
}

int pui_cc_tapped(void)
{
	int r = cc_tapped;
	cc_tapped = -1;
	return r;
}

// A thumbs-up (or thumbs-down) icon in a 12x12 box at x, y: a cuff, a fist, and the thumb
// sticking out of the fist's top (or bottom).
static void thumb_icon(float x, float y, bool down, u32 color)
{
	const float fist_y = down ? y : y + 5;
	C2D_DrawRectSolid(x, fist_y, 0.5f, 2.5f, 7, color);
	rrect(x + 3.5f, fist_y, 8.5f, 7, 2, color);
	rrect(x + 4.5f, down ? y + 5 : y, 3.5f, 7, 1.5f, color);
}

// A like or dislike button with its count ("" while it's loading or unknown)
static void vote_button(float x, bool down, const char* count)
{
	rrect(x, ACTION_Y, VOTE_W, ACTION_H, 7, C_PANEL);
	const bool known = count && count[0];
	const float content_w = 12 + (known ? 6 + text_w(count, 0.45f) : 0);
	const float left = x + (VOTE_W - content_w) / 2;
	thumb_icon(left, ACTION_Y + 5, down, C_WHITE);
	if (known) text(count, left + 18, ACTION_Y + 3, 0.45f, C_WHITE, C2D_AlignLeft);
}

// The progress bar (tap or drag to seek)
static void progress_bar(const PlayerStatus* st, const char* title)
{
	if (st->live)
	{
		// a live stream has nothing to seek in, so just show how long we've been watching
		rrect(BAR_X, BAR_Y + 2, 44, 18, 5, C_RED);
		text("LIVE", BAR_X + 22, BAR_Y + 3, 0.45f, C_WHITE, C2D_AlignCenter);
		char ago[24];
		fmt_dur(ago, sizeof(ago), (int)(st->pos_ms / 1000));
		char watching[48];
		snprintf(watching, sizeof(watching), "watching for %s", st->pos_ms >= 1000 ? ago : "0:00");
		text(watching, BAR_X + 52, BAR_Y + 4, 0.42f, C_MUTED, C2D_AlignLeft);
		return;
	}
	// while the bar is dragged, the knob and time follow the stylus instead of playback
	const u32 shown_ms = scrubbing ? (u32)(scrub_frac * (float)st->dur_ms) : st->pos_ms;
	float pos_frac = st->dur_ms ? (float)shown_ms / (float)st->dur_ms : 0;
	float buffered_frac = st->dur_ms ? ((float)st->pos_ms + st->buffered_s * 1000.0f) / (float)st->dur_ms : 0;
	if (pos_frac > 1) pos_frac = 1;
	if (buffered_frac > 1 || st->all_loaded) buffered_frac = 1;
	rrect(BAR_X, BAR_Y, BAR_W, BAR_H, BAR_H / 2, C_PANEL2);
	rrect(BAR_X, BAR_Y, BAR_W * buffered_frac, BAR_H, BAR_H / 2, RGBA(90, 98, 112, 255));
	rrect(BAR_X, BAR_Y, BAR_W * pos_frac + 1, BAR_H, BAR_H / 2, C_RED);
	if (scrubbing) C2D_DrawCircleSolid(BAR_X + BAR_W * pos_frac, BAR_Y + BAR_H / 2, 0.5f, 15, RGBA(255, 255, 255, 70));
	C2D_DrawCircleSolid(BAR_X + BAR_W * pos_frac, BAR_Y + BAR_H / 2, 0.5f, scrubbing ? 11 : 9, C_WHITE);
	char pos_text[16], dur_text[16];
	fmt_dur(pos_text, sizeof(pos_text), (int)(shown_ms / 1000));
	if (shown_ms < 1000) snprintf(pos_text, sizeof(pos_text), "0:00");
	if (st->dur_ms) fmt_dur(dur_text, sizeof(dur_text), (int)(st->dur_ms / 1000));
	else snprintf(dur_text, sizeof(dur_text), "--:--");
	text(pos_text, BAR_X, BAR_Y + 18, 0.42f, scrubbing ? C_WHITE : C_MUTED, C2D_AlignLeft);
	text(dur_text, BAR_X + BAR_W, BAR_Y + 18, 0.42f, C_MUTED, C2D_AlignRight);
}

// A row of the settings page: label on the left, value and arrow on the right; tapping it does what.
static float set_row(float y, const char* label, const char* value, u32 value_color, int what)
{
	rrect(PANEL_X, y, PANEL_W, SP_ROW_H, 8, C_PANEL);
	text(label, 20, y + 6, 0.5f, C_WHITE, C2D_AlignLeft);
	text(value, 286, y + 6, 0.47f, value_color, C2D_AlignRight);
	tri_right(296, y + SP_ROW_H / 2, 4, C_MUTED);
	if (set_row_count < SP_MAX_ROWS)
		set_rows[set_row_count++] = (typeof(set_rows[0])){ y, SP_ROW_H, PANEL_X, PANEL_W, what };
	return y + SP_ROW_STEP;
}

// One of the two switch tiles at the bottom of the settings page.
static void set_tile(float x, float y, const char* label, bool on, int what)
{
	rrect(x, y, HALF_W, SP_TILE_H, 8, C_PANEL);
	text(label, x + 10, y + 10, 0.44f, C_WHITE, C2D_AlignLeft);
	on_off_switch(x + HALF_W - SWITCH_W - 5, y + 6, on);
	if (set_row_count < SP_MAX_ROWS)
		set_rows[set_row_count++] = (typeof(set_rows[0])){ y, SP_TILE_H, (int)x, HALF_W, what };
}

static float slider_dot_x(int step)
{
	return SP_SLIDER_X0 + (float)(step + PUI_DEPTH_STEPS) * (SP_SLIDER_X1 - SP_SLIDER_X0) / (2 * PUI_DEPTH_STEPS);
}

// The playback settings page: subtitles and audio, 3D layout and depth (3D videos), Smooth motion
// and nerd stats. The video keeps playing above it.
static void settings_panel(const PlayerStatus* st)
{
	set_row_count = 0;
	set_slider_y = -1;
	text("Settings", 16, 8, 0.6f, C_WHITE, C2D_AlignLeft);
	rrect(SP_CLOSE_X, 6, SP_CLOSE_W, SP_CLOSE_H, 8, C_PANEL);
	text("Close", SP_CLOSE_X + SP_CLOSE_W / 2, 10, 0.47f, C_WHITE, C2D_AlignCenter);

	float y = SP_ROW_Y;
	if (st->cc_label && !st->cc_none)
	{
		const char* value = st->cc_on ? st->cc_label + (strncmp(st->cc_label, "CC ", 3) ? 0 : 3) : "Off";
		y = set_row(y, "Subtitles & audio", value, st->cc_on ? C_CYAN : C_MUTED, PUI_SET_CC);
	}
	if (st->can_switch) y = set_row(y, "3D layout", st->mode_name, C_CYAN, PUI_SET_LAYOUT);
	if (st->show_depth && st->stereo)
	{
		// the depth slider: a dot per step of Up/Down, the middle one the video's own depth
		char value[40];
		if (st->auto_px) snprintf(value, sizeof(value), "%+d  \xC2\xB7  auto %+d", st->depth_step, st->auto_px);
		else snprintf(value, sizeof(value), "%+d", st->depth_step);
		text("Depth", 20, y + 2, 0.5f, C_WHITE, C2D_AlignLeft);
		text(value, 300, y + 3, 0.45f, C_CYAN, C2D_AlignRight);
		const float line_y = y + 36;
		set_slider_y = line_y;
		const float knob_x = slider_dot_x(st->depth_step), zero_x = slider_dot_x(0);
		rrect(SP_SLIDER_X0, line_y - 2, SP_SLIDER_X1 - SP_SLIDER_X0, 4, 2, C_PANEL2);
		rrect(knob_x < zero_x ? knob_x : zero_x, line_y - 2, fabsf(knob_x - zero_x), 4, 2, C_CYAN);
		for (int step = -PUI_DEPTH_STEPS; step <= PUI_DEPTH_STEPS; step++)
			C2D_DrawCircleSolid(slider_dot_x(step), line_y, 0.5f, step == 0 ? 4.5f : 3.0f,
			                    step == 0                                                                ? C_WHITE
			                    : (step < 0) == (st->depth_step < 0) && abs(step) <= abs(st->depth_step) ? C_CYAN
			                                                                                             : C_MUTED);
		C2D_DrawCircleSolid(knob_x, line_y, 0.5f, set_dragging ? 11 : 9, C_WHITE);
		text("closer", SP_SLIDER_X0 - 6, line_y + 10, 0.38f, C_MUTED, C2D_AlignLeft);
		text("further", SP_SLIDER_X1 + 6, line_y + 10, 0.38f, C_MUTED, C2D_AlignRight);
		y += SP_DEPTH_H + 8;
	}
	set_tile(HALF_L_X, y, "Smooth motion", st->smooth_on, PUI_SET_SMOOTH);
	set_tile(HALF_R_X, y, "Nerd stats", st->stats_on, PUI_SET_STATS);
}

int pui_settings_tapped(void)
{
	const int what = set_tapped;
	set_tapped = PUI_SET_NONE;
	return what;
}

// The depth step under the stylus on the slider
static int slider_step(int x)
{
	const float per = (float)(SP_SLIDER_X1 - SP_SLIDER_X0) / (2 * PUI_DEPTH_STEPS);
	int step = (int)lroundf((x - SP_SLIDER_X0) / per) - PUI_DEPTH_STEPS;
	return step < -PUI_DEPTH_STEPS ? -PUI_DEPTH_STEPS : step > PUI_DEPTH_STEPS ? PUI_DEPTH_STEPS : step;
}

// The playback remote. The caller has already begun a frame and the bottom scene.
static void status_bottom(const PlayerStatus* st)
{
	status_cc_menu = st->cc_menu;
	status_cm_open = st->cm_open;
	status_set_open = st->set_open && !st->cm_open && !st->cc_menu;
	if (status_set_open)
	{
		settings_panel(st);
		return;
	}
	if (st->cm_open)
	{
		comments_panel(st);
		return;
	}
	if (st->cc_menu)
	{
		status_cc_top = st->cc_top;
		cc_list(st);
		return;
	}
	// the state chip on the left, the 3D mode on the right
	const char* state = st->buffering ? "Buffering" : st->paused ? "Paused" : "Playing";
	u32 chip_color = st->buffering ? C_AMBER : st->paused ? C_MUTED : C_RED;
	float chip_w = text_w(state, 0.45f) + 14;
	rrect(12, 10, chip_w, 18, 5, chip_color);
	text(state, 12 + 7, 11, 0.45f, st->buffering ? RGBA(65, 36, 2, 255) : C_WHITE, C2D_AlignLeft);
	text(st->mode_name, 308, 11, 0.45f, st->stereo ? C_CYAN : C_MUTED, C2D_AlignRight);

	char title[160];
	fit_to_width(title, sizeof(title), st->title ? st->title : "", 0.52f, 296);
	text(title, 12, 32, 0.52f, C_WHITE, C2D_AlignLeft);

	// the channel and views
	if (st->channel && st->channel[0])
	{
		char meta[128], who[96];
		const bool has_views = st->views && st->views[0];
		snprintf(meta, sizeof(meta), "%s%s%s", st->channel, has_views ? "  \xC2\xB7  " : "",
		         has_views ? st->views : "");
		fit_to_width(who, sizeof(who), meta, 0.45f, 288);
		text(who, 16, META_Y, 0.45f, C_MUTED, C2D_AlignLeft);
	}

	// for YouTube videos: likes, dislikes and the comments
	status_actions = st->comments_ok;
	if (st->comments_ok)
	{
		vote_button(LIKE_X, false, st->likes);
		vote_button(DISLIKE_X, true, st->dislikes);
		rrect(COMMENTS_X, ACTION_Y, COMMENTS_W, ACTION_H, 7, C_PANEL);
		text("Comments", COMMENTS_X + COMMENTS_W / 2 - 4, ACTION_Y + 3, 0.45f, C_CYAN, C2D_AlignCenter);
		tri_right(COMMENTS_X + COMMENTS_W - 12, ACTION_Y + ACTION_H / 2, 4, C_CYAN);
	}

	// hint line
	if (osGetTime() < vote_note_until)
		text("Voting needs a YouTube account", BOT_CX, HINT_Y, 0.45f, C_CYAN, C2D_AlignCenter);
	else if (st->stereo && osGet3DSliderState() <= 0.0f)
		text("Raise the 3D slider to see depth", BOT_CX, HINT_Y, 0.45f, C_CYAN, C2D_AlignCenter);
	else if (!st->audio_ok) text("No sound: dump the DSP firmware", BOT_CX, HINT_Y, 0.45f, C_AMBER, C2D_AlignCenter);
	else if (st->errors)
	{
		char err_text[48];
		snprintf(err_text, sizeof(err_text), "Decode errors: %lu", (unsigned long)st->errors);
		text(err_text, BOT_CX, HINT_Y, 0.42f, C_AMBER, C2D_AlignCenter);
	}
	else
	{
		// how much is buffered; with nerd stats on also free memory and the MVD buffer size,
		// to keep an eye on the RAM budget
		char line[96], amount[24];
		if (st->all_loaded) snprintf(amount, sizeof(amount), "Fully loaded");
		else snprintf(amount, sizeof(amount), "Buffered %.0f s", st->buffered_s);
		if (app.nerd_stats)
			snprintf(line, sizeof(line), "%s  \xC2\xB7  free %.1f + %.0f MB  \xC2\xB7  MVD %.1f MB", amount,
			         st->linear_free_kb / 1024.0f, st->heap_free_kb / 1024.0f, st->mvd_workbuf_kb / 1024.0f);
		else snprintf(line, sizeof(line), "%s", amount);
		text(line, BOT_CX, HINT_Y, 0.4f, C_MUTED, C2D_AlignCenter);
	}

	progress_bar(st, title);

	// transport buttons; a live stream only gets pause, since jumping would leave the stream
	rrect(PAUSE_X, PAUSE_Y, PAUSE_W, PAUSE_H, 12, C_RED);
	if (st->paused) tri_right(PAUSE_X + 42, BTN_Y + SEEK_BTN_H / 2, 11, C_WHITE);
	else
	{
		C2D_DrawRectSolid(PAUSE_X + 36, BTN_Y + 8, 0.5f, 8, 20, C_WHITE);
		C2D_DrawRectSolid(PAUSE_X + 52, BTN_Y + 8, 0.5f, 8, 20, C_WHITE);
	}
	if (!st->live)
	{
		rrect(REWIND_X, BTN_Y, SEEK_BTN_W, SEEK_BTN_H, 10, C_PANEL);
		tri_left(REWIND_X + 30, BTN_Y + SEEK_BTN_H / 2, 7, C_WHITE);
		tri_left(REWIND_X + 42, BTN_Y + SEEK_BTN_H / 2, 7, C_WHITE);
		text("10", REWIND_X + 62, BTN_Y + 9, 0.45f, C_MUTED, C2D_AlignCenter);
		rrect(FORWARD_X, BTN_Y, SEEK_BTN_W, SEEK_BTN_H, 10, C_PANEL);
		text("10", FORWARD_X + 18, BTN_Y + 9, 0.45f, C_MUTED, C2D_AlignCenter);
		tri_right(FORWARD_X + 38, BTN_Y + SEEK_BTN_H / 2, 7, C_WHITE);
		tri_right(FORWARD_X + 50, BTN_Y + SEEK_BTN_H / 2, 7, C_WHITE);
	}

	status_live = st->live;
	// Back, and Settings for subtitles, 3D layout, depth, Smooth motion and nerd stats
	rrect(BACK_X, BACK_Y, BACK_W, BACK_H, 8, C_PANEL);
	text("Back", BACK_X + BACK_W / 2, BACK_Y + 5, 0.5f, C_WHITE, C2D_AlignCenter);
	rrect(SETTINGS_X, BACK_Y, SETTINGS_W, BACK_H, 8, C_PANEL);
	text("Settings", SETTINGS_X + SETTINGS_W / 2, BACK_Y + 5, 0.5f, C_CYAN, C2D_AlignCenter);
}

// ---------------------------------------------------------------- direct mode video

// Draws one eye's part of the picture, fitted to 400x240 with that eye's aspect ratio.
// The picture starts at the texture's top, so picture row y is at v = 1 - y / tex_h.
static void video_eye(const PlayerVideoFrame* v, int eye)
{
	float iw = v->img_w, ih = v->img_h;
	float sx = 0, sy = 0, sw = iw, sh = ih, aspect = iw / ih;
	int half = v->swap_eyes ? eye ^ 1 : eye;   // which half of the frame this eye shows
	if (v->layout == VL_SBS)
	{
		sw = iw / 2;
		sx = half ? sw : 0;
		aspect = v->full ? sw / ih : iw / ih;   // half-SBS halves are squeezed 2x
	}
	else if (v->layout == VL_TB)
	{
		sh = ih / 2;
		sy = half ? sh : 0;
		aspect = v->full ? iw / sh : iw / ih;
	}
	float dw = (float)TOP_W, dh = (float)TOP_W / aspect;
	if (dh > (float)TOP_H)
	{
		dh = (float)TOP_H;
		dw = (float)TOP_H * aspect;
	}
	// this eye's whole half before the convergence crop, which Smooth motion's mesh covers
	const float hx = sx, hy = sy, hw = sw, hh = sh;

	// Convergence: each eye shows a window of its half, offset by conv picture pixels and
	// stretched back to full width, so every point's on-screen disparity shifts by the same
	// amount. It's the same trick thumb_eye uses for thumbnails, and only applies while the
	// right eye is shown.
	float convergence = v->layout != VL_2D && osGet3DSliderState() > 0.0f ? v->conv : 0.0f;
	if (convergence > sw / 8) convergence = sw / 8;
	if (convergence < -sw / 8) convergence = -sw / 8;
	if (convergence != 0.0f)
	{
		float conv_abs = convergence < 0 ? -convergence : convergence;
		bool crop_left = (eye == 1) == (convergence > 0);   // positive conv: the right eye drops its left edge
		if (crop_left) sx += conv_abs;
		sw -= conv_abs;
	}

	Tex3DS_SubTexture sub = {
		.width = (u16)sw,
		.height = (u16)sh,
		.left = sx / v->tex_w,
		.right = (sx + sw) / v->tex_w,
		.top = 1.0f - sy / v->tex_h,
		.bottom = 1.0f - (sy + sh) / v->tex_h,
	};
	if (v->tex2 && v->mvx && vmesh_ready())
	{
		// Motion: Smooth draws an in-between picture. Both the current and the next picture
		// become meshes, moved along the motion vectors to where their content would be at
		// this moment, and the next one is blended over the current by how far along we are.
		// The vectors are measured on one eye only and applied to each eye's own half, so
		// both eyes move identically. The mesh maps this eye's window (including
		// convergence) onto the screen.
		VMeshDraw mesh = {
			.tex = (C3D_Tex*)v->tex,
			.tex_w = v->tex_w,
			.tex_h = v->tex_h,
			.org_x = hx,
			.org_y = hy,
			.pic_w = hw,
			.pic_h = hh,
			.vw = v->mvw,
			.vh = v->mvh,
			.cell = v->mcell,
			.vx = v->mvx,
			.vy = v->mvy,
			.s = v->mix,
			.src_x = sx,
			.src_y = sy,
			.scale_x = dw / sw,
			.scale_y = dh / sh,
			.dst_x = (TOP_W - dw) / 2,
			.dst_y = (TOP_H - dh) / 2,
			.alpha = 1.0f,
		};
		vmesh_draw(&mesh);
		mesh.tex = (C3D_Tex*)v->tex2;
		mesh.s = -(1.0f - v->mix);
		mesh.alpha = v->mix;
		vmesh_draw(&mesh);
		return;
	}
	C2D_Image img = { (C3D_Tex*)v->tex, &sub };
	C2D_DrawImageAt(img, (TOP_W - dw) / 2, (TOP_H - dh) / 2, 0.5f, NULL, dw / sw, dh / sh);
	if (v->tex2 && v->mix > 0.0f)
	{
		// no motion vectors (e.g. at a cut): just cross-fade to the next picture. Both
		// textures share layout and size, so the same subtexture works for both.
		C2D_ImageTint tint;
		C2D_AlphaImageTint(&tint, v->mix);
		C2D_Image img2 = { (C3D_Tex*)v->tex2, &sub };
		C2D_DrawImageAt(img2, (TOP_W - dw) / 2, (TOP_H - dh) / 2, 0.5f, &tint, dw / sw, dh / sh);
	}
}

#define STATS_LINES    6    // the nerd stats box shows at most this many lines
#define STATS_LINE_MAX 95   // of at most this many bytes each

// Nerd stats and the paused / buffering marks, drawn at zero depth so they sit on the screen plane
static void video_overlay(const PlayerVideoFrame* v)
{
	if (v->stats && v->tex)
	{
		// Settings "Nerd stats": a box of right-aligned lines in the top-right corner
		char lines[STATS_LINES][STATS_LINE_MAX + 1];
		int count = 0;
		for (const char* p = v->stats; *p && count < STATS_LINES;)
		{
			const char* nl = strchr(p, '\n');
			const size_t len = nl ? (size_t)(nl - p) : strlen(p);
			snprintf(lines[count++], sizeof(lines[0]), "%.*s", (int)(len < STATS_LINE_MAX ? len : STATS_LINE_MAX), p);
			p += len + (nl ? 1 : 0);
		}
		float box_w = 0;
		for (int i = 0; i < count; i++)
		{
			const float line_w = text_w(lines[i], 0.4f);
			if (line_w > box_w) box_w = line_w;
		}
		rrect(396 - box_w - 10, 4, box_w + 10, 4 + 13.0f * count, 4, RGBA(0, 0, 0, 150));
		for (int i = 0; i < count; i++)
			text(lines[i], 391, 5 + 13.0f * i, 0.4f, i == 0 ? C_WHITE : RGBA(215, 220, 230, 255), C2D_AlignRight);
	}
	if (v->buffering || !v->tex)
	{
		C2D_DrawCircleSolid(TOP_CX, 120, 0.5f, 26, RGBA(0, 0, 0, 150));
		spinner(TOP_CX, 120, 15, C_WHITE);
		if (v->note)
		{
			// a seek's target under the spinner, over the last picture
			float box_w = text_w(v->note, 0.5f) + 16;
			rrect(TOP_CX - box_w / 2, 152, box_w, 20, 6, RGBA(0, 0, 0, 160));
			text(v->note, TOP_CX, 154, 0.5f, C_WHITE, C2D_AlignCenter);
		}
	}
	else if (v->paused)
	{
		rrect(14, 14, 34, 34, 8, RGBA(0, 0, 0, 150));
		C2D_DrawRectSolid(23, 21, 0.5f, 6, 20, C_WHITE);
		C2D_DrawRectSolid(34, 21, 0.5f, 6, 20, C_WHITE);
	}
}

// Subtitle lines, parsed once per frame and drawn in each eye at the same place (so they sit
// on the screen plane), bottom-aligned on dark boxes.
#define SUB_MAX_LINES 4
#define SUB_SCALE     0.55f
#define SUB_LINE_H    17.0f
#define SUB_WRAP_W    372.0f

typedef struct {
	char line[SUB_MAX_LINES][256];
	bool crisp[SUB_MAX_LINES];   // drawn with uifont; otherwise with the system font via t
	C2D_Text t[SUB_MAX_LINES];
	float w[SUB_MAX_LINES];
	int rows[SUB_MAX_LINES];   // screen rows each line wraps to
	int n;
} SubLines;

static void sub_prepare(const char* s, SubLines* out)
{
	out->n = 0;
	while (s && *s && out->n < SUB_MAX_LINES)
	{
		char* line = out->line[out->n];
		size_t len = strcspn(s, "\n");
		size_t keep = len < sizeof(out->line[0]) ? len : sizeof(out->line[0]) - 1;
		while (keep > 0 && keep < len && ((unsigned char)s[keep] & 0xC0) == 0x80)
			keep--;   // don't split a UTF-8 character
		memcpy(line, s, keep);
		line[keep] = 0;
		float w = 0, h = 0;
		out->crisp[out->n] = uifont_width(line, SUB_SCALE, &w);
		if (!out->crisp[out->n])
		{
			C2D_Text* t = &out->t[out->n];
			C2D_TextParse(t, frame_text, line);
			C2D_TextOptimize(t);
			C2D_TextGetDimensions(t, SUB_SCALE, SUB_SCALE, &w, &h);
		}
		out->w[out->n] = w > SUB_WRAP_W ? SUB_WRAP_W : w;
		out->rows[out->n] = (int)(w / SUB_WRAP_W) + 1;
		out->n++;
		s += len;
		if (*s == '\n') s++;
	}
}

static void sub_draw(const SubLines* subs)
{
	float y = 236.0f;
	for (int i = subs->n - 1; i >= 0; i--)
	{
		float h = subs->rows[i] * SUB_LINE_H;
		y -= h + 3;
		rrect(TOP_CX - subs->w[i] / 2 - 6, y - 1, subs->w[i] + 12, h + 3, 4, RGBA(0, 0, 0, 170));
		if (subs->crisp[i])
			uifont_draw_wrap(subs->line[i], TOP_CX, y, 0.5f, SUB_SCALE, C_WHITE, UIF_CENTER, SUB_WRAP_W);
		else
			C2D_DrawText(&subs->t[i], C2D_WithColor | C2D_WordWrap | C2D_AlignCenter, TOP_CX, y, 0.5f, SUB_SCALE,
			             SUB_SCALE, C_WHITE, SUB_WRAP_W);
	}
}

// libctru's GPU command queue knows a command has finished by counting GPU interrupts (one
// PSC0, P3D or PPF each). On hardware, one interrupt went missing during a 640x360 video. The
// queue then stayed "running" for good, and citro3d's next wait on it (C3D_SyncTextureCopy,
// C3D_FrameBegin(0)) never returned, freezing the console a second into the video every time.
//
// So waits on the queue have a time limit (everything queued takes only a few ms), and a queue
// stuck past it gets the interrupt it's missing: we submit a tiny memory fill straight to GSP,
// outside the queue, and its PSC0 counts in place of the lost one.
//
// Also beware: C3D_Sync* inside a frame only queue their transfer. Outside a frame they also
// wait for a PPF event, and an earlier transfer's event can satisfy that before the copy is done.
static u32 gpu_recoveries;
static bool gpu_dead;
static u32* gpu_scratch;   // VRAM the recovery fill writes

static void gpu_kick(void)
{
	if (!gpu_scratch) gpu_scratch = (u32*)vramAlloc(256);
	if (!gpu_scratch) return;
	u32 cmd[8] = { 0x02, (u32)gpu_scratch, 0, (u32)(gpu_scratch + 64), 0, 0, 0, GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH };
	gspSubmitGxCommand(cmd);
}

bool pui_gpu_begin(u32 timeout_ms)
{
	u64 since = osGetTime();
	int kicks = 0;
	while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK))
	{
		if (osGetTime() - since >= timeout_ms)
		{
			if (kicks++ == 3)
			{
				gpu_dead = true;
				return false;
			}
			gpu_recoveries++;
			gpu_kick();
			since = osGetTime();
		}
		svcSleepThread(100000);
	}
	return true;
}

void pui_gpu_end(void)
{
	// GX_CMDLIST_FLUSH skips flushing the CPU cache for the whole linear heap, which
	// C3D_FrameEnd(0) would do for the vertex data. Not needed here: the copies only read
	// memory that MVD or the GPU wrote.
	C3D_FrameEnd(GX_CMDLIST_FLUSH);
}

u32 pui_gpu_recoveries(void) { return gpu_recoveries; }
bool pui_gpu_dead(void) { return gpu_dead; }

void pui_video(const PlayerVideoFrame* v, const PlayerStatus* st)
{
	if (closing()) return;
	app.frame++;
	// Don't use C3D_FRAME_SYNCDRAW here. It waits for both screens to refresh, and since they
	// refresh at different moments, the picture would land one or two refreshes late depending
	// on when we were called (on hardware, 30% of frames ended up a refresh off). Instead the
	// player waits for the top screen's refresh itself and calls this right after it.
	if (!pui_gpu_begin(200)) return;
	vmesh_frame_reset();
	C2D_TextBufClear(frame_text);
	SubLines subs;
	sub_prepare(v->subtitle, &subs);
	// the right eye is only drawn (and only visible) while the 3D slider is up
	int eyes = osGet3DSliderState() > 0.0f ? 2 : 1;
	for (int eye = 0; eye < eyes; eye++)
	{
		C3D_RenderTarget* t = eye ? top_r : top_l;
		C2D_TargetClear(t, RGBA(0, 0, 0, 255));
		begin_scene(t);
		if (v->tex) video_eye(v, eye);
		if (subs.n) sub_draw(&subs);
		video_overlay(v);
	}
	if (st)
	{
		C2D_TargetClear(bot, C_BG2);
		begin_scene(bot);
		status_bottom(st);
	}
	C3D_FrameEnd(0);
}

// the fraction of the video under the stylus at x on the seek bar
static float bar_frac(int x)
{
	float f = (float)(x - BAR_X) / BAR_W;
	return f < 0 ? 0 : f > 0.99f ? 0.99f : f;
}

u32 pui_touch(u32 dur_ms, s64* seek_ms)
{
	*seek_ms = -1;
	touchPosition tp;
	if (scrubbing)
	{
		// dragging the seek bar: follow the stylus anywhere on the screen, seek when it lifts
		if (hidKeysHeld() & KEY_TOUCH)
		{
			hidTouchRead(&tp);
			scrub_frac = bar_frac(tp.px);
		}
		else
		{
			scrubbing = false;
			if (dur_ms) *seek_ms = (s64)(scrub_frac * (float)dur_ms);
		}
		return 0;
	}
	if (set_dragging)
	{
		// dragging the depth slider: follow the stylus from dot to dot
		if (status_set_open && (hidKeysHeld() & KEY_TOUCH))
		{
			hidTouchRead(&tp);
			set_tapped = PUI_SET_DEPTH + slider_step(tp.px) + PUI_DEPTH_STEPS;
		}
		else set_dragging = false;
		return 0;
	}
	if (status_cm_open && cm_drag)
	{
		// dragging the comments: the list follows the stylus
		if (hidKeysHeld() & KEY_TOUCH)
		{
			hidTouchRead(&tp);
			cm_scroll = cm_drag_from - (float)(tp.py - cm_drag_y);
			cm_moving = true;
		}
		else cm_drag = false;
		return 0;
	}
	if (!touched(&tp)) return 0;
	int x = tp.px, y = tp.py;
	if (status_cm_open)
	{
		if (in_rect(x, y, LIST_UP_X, LIST_BAR_Y, LIST_ARROW_W, LIST_BAR_H))
			pui_comments_scroll(-(CM_BOTTOM - CM_TOP) * 0.8f);
		else if (in_rect(x, y, LIST_CLOSE_X, LIST_BAR_Y, LIST_CLOSE_W, LIST_BAR_H)) return KEY_B;
		else if (in_rect(x, y, LIST_DOWN_X, LIST_BAR_Y, LIST_ARROW_W, LIST_BAR_H))
			pui_comments_scroll((CM_BOTTOM - CM_TOP) * 0.8f);
		else if (y >= CM_TOP && y < CM_BOTTOM)
		{
			cm_drag = true;
			cm_drag_y = y;
			cm_drag_from = cm_scroll;
		}
		return 0;
	}
	if (status_set_open)
	{
		if (in_rect(x, y, SP_CLOSE_X, 6, SP_CLOSE_W, SP_CLOSE_H)) return KEY_B;
		if (set_slider_y >= 0 && y >= set_slider_y - 18 && y <= set_slider_y + 18)
		{
			set_dragging = true;   // a tap picks a dot; dragging follows the stylus
			set_tapped = PUI_SET_DEPTH + slider_step(x) + PUI_DEPTH_STEPS;
			return 0;
		}
		for (int r = 0; r < set_row_count; r++)
			if (in_rect(x, y, set_rows[r].x, (int)set_rows[r].y, set_rows[r].w, (int)set_rows[r].h))
				set_tapped = set_rows[r].what;
		return 0;
	}
	if (status_cc_menu)
	{
		for (int r = status_cc_top; r < status_cc_rows && r < status_cc_top + PUI_CC_ROWS; r++)
			if (in_rect(x, y, 12, CC_LIST_Y + (r - status_cc_top) * CC_LIST_ROW_H, 296, CC_LIST_ROW_H - 4))
				cc_tapped = r;
		if (in_rect(x, y, LIST_UP_X, LIST_BAR_Y, LIST_ARROW_W, LIST_BAR_H)) return KEY_DUP;
		if (in_rect(x, y, LIST_CLOSE_X, LIST_BAR_Y, LIST_CLOSE_W, LIST_BAR_H)) return KEY_B;
		if (in_rect(x, y, LIST_DOWN_X, LIST_BAR_Y, LIST_ARROW_W, LIST_BAR_H)) return KEY_DDOWN;
		return 0;
	}
	if (dur_ms && in_rect(x, y, BAR_X - 8, BAR_Y - 12, BAR_W + 16, BAR_H + 26))
	{
		scrubbing = true;   // a tap seeks when lifted, a drag follows the stylus first
		scrub_frac = bar_frac(x);
		return 0;
	}
	if (status_actions && in_rect(x, y, COMMENTS_X, ACTION_Y, COMMENTS_W, ACTION_H)) return KEY_SELECT;
	if (status_actions &&
	    (in_rect(x, y, LIKE_X, ACTION_Y, VOTE_W, ACTION_H) || in_rect(x, y, DISLIKE_X, ACTION_Y, VOTE_W, ACTION_H)))
	{
		vote_note_until = osGetTime() + VOTE_NOTE_MS;   // voting needs a signed-in account, which we don't have
		return 0;
	}
	if (!status_live && in_rect(x, y, REWIND_X, BTN_Y, SEEK_BTN_W, SEEK_BTN_H)) return KEY_DLEFT;
	if (in_rect(x, y, PAUSE_X, PAUSE_Y, PAUSE_W, PAUSE_H)) return KEY_A;
	if (!status_live && in_rect(x, y, FORWARD_X, BTN_Y, SEEK_BTN_W, SEEK_BTN_H)) return KEY_DRIGHT;
	if (in_rect(x, y, BACK_X, BACK_Y, BACK_W, BACK_H)) return KEY_B;
	if (in_rect(x, y, SETTINGS_X, BACK_Y, SETTINGS_W, BACK_H)) return KEY_START;   // opens the settings page
	return 0;
}

#define MSG_BTN_Y 176   // pui_message's buttons: Back and yes_label side by side, or one OK
#define MSG_BTN_H 44

bool pui_message(const char* title, const char* msg, const char* yes_label, const char* no_label)
{
	if (closing()) return false;
	power_bottom_backlight(true);
	while (aptMainLoop())
	{
		hidScanInput();
		u32 keys = hidKeysDown();
		if ((keys & KEY_A) && yes_label) return true;
		if (keys & (KEY_B | (yes_label ? 0 : KEY_A))) return false;
		touchPosition tp;
		if (touched(&tp))
		{
			if (yes_label)
			{
				if (in_rect(tp.px, tp.py, HALF_R_X, MSG_BTN_Y, HALF_W, MSG_BTN_H)) return true;
				if (in_rect(tp.px, tp.py, HALF_L_X, MSG_BTN_Y, HALF_W, MSG_BTN_H)) return false;
			}
			else if (in_rect(tp.px, tp.py, PANEL_X, MSG_BTN_Y, PANEL_W, MSG_BTN_H)) return false;
		}

		app.frame++;
		update_bg();
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TextBufClear(frame_text);
		// over a video, show its card; in the menus, the screen we came from (e.g. the Videos tab)
		if (top_is_ours()) draw_top(g_playing ? top_now_playing : NULL, false, g_playing ? NULL : top_browse);
		C2D_TargetClear(bot, C_BG2);
		begin_scene(bot);
		text(title, BOT_CX, 26, 0.7f, C_RED, C2D_AlignCenter);
		char shown[320];
		clip(shown, sizeof(shown), msg ? msg : "", 300);
		text_wrap(shown, BOT_CX, 66, 0.5f, C_WHITE, C2D_AlignCenter, 290);
		if (yes_label)
		{
			rrect(HALF_L_X, MSG_BTN_Y, HALF_W, MSG_BTN_H, 10, C_PANEL);
			text(no_label ? no_label : "Back", HALF_L_X + HALF_W / 2, MSG_BTN_Y + 12, 0.6f, C_WHITE, C2D_AlignCenter);
			rrect(HALF_R_X, MSG_BTN_Y, HALF_W, MSG_BTN_H, 10, C_RED);
			text(yes_label, HALF_R_X + HALF_W / 2, MSG_BTN_Y + 12, 0.6f, C_WHITE, C2D_AlignCenter);
		}
		else
		{
			rrect(PANEL_X, MSG_BTN_Y, PANEL_W, MSG_BTN_H, 10, C_PANEL);
			text(no_label ? no_label : "OK", PANEL_X + PANEL_W / 2, MSG_BTN_Y + 12, 0.6f, C_WHITE, C2D_AlignCenter);
		}
		C3D_FrameEnd(0);
	}
	return false;
}
