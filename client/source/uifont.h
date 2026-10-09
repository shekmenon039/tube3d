#pragma once
// Crisp UI text. Noto Sans is pre-rendered by tools/make_font.py at every pixel size the UI
// uses (data/ui_font.bin) and drawn 1:1 at whole-pixel positions. We don't rely on the system
// font because it's a ~30 px bitmap that citro2d shrinks on the GPU, which leaves soft, uneven
// strokes at the UI's small sizes. Text with a character the atlas lacks (Japanese, emoji...)
// returns false so the caller can draw it with the system font instead.
//
// Sizes follow citro2d's text scale: scale 1.0 is ~24 px Noto Sans, about as wide as the
// system font at scale 1, so layouts measured for the system font still fit. y is the top
// of the line, as for C2D_DrawText (the baseline sits the system font's ascent below it).
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool uifont_init(void);   // false if there's no atlas; the system font is then used for everything
void uifont_exit(void);
bool uifont_ready(void);   // true once the atlas is loaded

enum { UIF_LEFT = 0, UIF_CENTER = 1, UIF_RIGHT = 2 };

// Width of s at scale; false if s has a character the atlas lacks.
bool uifont_width(const char* s, float scale, float* w);

// Draws one line. False (nothing drawn) if s has a character the atlas lacks.
bool uifont_draw(const char* s, float x, float y, float z, float scale, uint32_t color, int align);

// Draws s word-wrapped to width (lines also break at '\n'), aligned within [x, x+width] for
// UIF_LEFT or around x for UIF_CENTER (as C2D_DrawText with C2D_WordWrap). Returns the
// number of lines, or 0 (nothing drawn) if s has a character the atlas lacks.
int uifont_draw_wrap(const char* s, float x, float y, float z, float scale, uint32_t color, int align, float width);

#ifdef __cplusplus
}
#endif
