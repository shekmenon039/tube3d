#pragma once
// The Tube3D logo, pre-rendered by tools/make_logo.py into data/logo.bin. It has two layers:
// the base (a red play square and "tube") and the pop layer (an extruded cyan "3D"). Both come
// in two heights and are drawn 1:1 at whole pixels. Each layer gets its own per-eye shift, so
// the "3D" can float in front of the rest.
#include <stdbool.h>

enum { LOGO_HEADER = 0, LOGO_HERO = 1 };   // 16 px tall for the header, 44 px for the empty Home screen

bool logo_init(void);   // false: draw a text logo instead
void logo_exit(void);
// Pixel size of the logo at LOGO_HEADER or LOGO_HERO (0 x 0 when it isn't loaded).
void logo_size(int size, int* w, int* h);
// Draws the logo with its top-left at (x, y). base_dx and pop_dx are this eye's shift for
// each layer.
bool logo_draw(int size, float x, float y, float base_dx, float pop_dx);
