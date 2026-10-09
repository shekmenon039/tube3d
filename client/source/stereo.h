#pragma once
// Stereo helpers with no 3DS dependencies, so they can be unit-tested on a PC.
#include <stdbool.h>
#include <stdint.h>

// Global horizontal disparity between two same-sized 8-bit luminance images: returns the
// shift d (in pixels, -max_d..max_d) for which right(x + d, y) best matches left(x, y), i.e.
// how far the right eye's picture sits to the right of the left eye's. Rows in the top and
// bottom sixth are ignored (letterbox bars). Ties go to the smaller shift, so flat or empty
// pictures give 0. If pair is not NULL, *pair tells whether the two pictures are a stereo
// pair at all (one scene, only shifted) or unrelated, like the two halves of a 2D frame.
int stereo_analyse(const uint8_t* left, const uint8_t* right, int w, int h, int max_d, bool* pair);

// True if the picture has enough detail to judge (dark or flat frames don't: any two of
// them look like a stereo pair).
bool stereo_textured(const uint8_t* img, int w, int h);

// Shrinks the two eyes of an RGB565 frame into small luminance images, ow x oh each, the way
// the direct player measures them. Each output pixel averages its whole area of the eye's
// picture; picking single pixels instead aliased fine detail like leaves differently in the
// two eyes and hid real top-bottom pairs.
// px and stride describe the frame (stride in pixels), iw x ih is the visible picture, tb
// means top-bottom halves rather than side-by-side, and swap means the right eye is the first
// half. Returns the width of one eye's picture, so callers can scale shifts back up.
uint32_t stereo_sample_eyes(const uint16_t* px, uint32_t stride, uint32_t iw, uint32_t ih, bool tb, bool swap,
                            uint8_t* left, uint8_t* right, uint32_t ow, uint32_t oh);

// Luminance (0-255) of an RGB565 pixel.
static inline uint8_t stereo_luma565(uint16_t p)
{
	return (uint8_t)((((p >> 11) & 31) * 16 + ((p >> 5) & 63) * 20 + (p & 31) * 8) >> 3);
}
