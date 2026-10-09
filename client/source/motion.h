#pragma once
// Motion estimation for frame interpolation (the Smooth motion setting). It's pure C with no
// 3DS calls, so it can be tested on real video frames on a PC.
//
// Each decoded picture is shrunk to a small luminance map, ME_SCALE times smaller each way.
// For two consecutive pictures A and B, the map is cut into ME_BLOCK x ME_BLOCK blocks and
// each block of A is looked for in B. The search starts from the neighbours' vectors and the
// previous field's, then refines with a small diamond search and a sub-pixel fit. The result
// is how far each block's content moves from A to B.
//
// A picture at time t between them is drawn as two meshes (see me_vertex_vectors): A's pixels
// moved forward by t * v, B's moved back by (1 - t) * v, and B blended over A with opacity t.
// Blocks with a poor best match (something uncovered, or a scene cut) get no motion and simply
// cross-fade. If too many blocks are like that, the pair is treated as a cut and not
// interpolated at all.
#include <stdbool.h>
#include <stdint.h>

#ifndef ME_SCALE
#define ME_SCALE 4   // picture pixels per luma-map pixel
#endif
#ifndef ME_BLOCK
#define ME_BLOCK 8   // luma-map pixels per block side (32 picture pixels)
#endif
#ifndef ME_MAX_W
#define ME_MAX_W 256   // largest luma map (a 1024-px picture)
#define ME_MAX_H 128
#endif
#define ME_MAX_BW (ME_MAX_W / ME_BLOCK)
#define ME_MAX_BH (ME_MAX_H / ME_BLOCK)

typedef struct {
	int bw, bh;                       // blocks across and down
	float vx[ME_MAX_BH][ME_MAX_BW];   // motion from A to B, in luma-map pixels
	float vy[ME_MAX_BH][ME_MAX_BW];
	uint8_t ok[ME_MAX_BH][ME_MAX_BW];   // the match was good enough to use
	int good;                           // blocks with ok set
	bool usable;                        // enough good blocks: interpolate this pair
	float gx, gy;                       // global motion (median of the trusted blocks), luma pixels
	float quality;                      // share of the blocks with detail that matched (0..1)
} MotionField;

// Builds the luma map of an RGB565 picture (stride in pixels). Each output pixel averages a
// ME_SCALE x ME_SCALE area, so the map is w / ME_SCALE by h / ME_SCALE, capped at ME_MAX_*.
void me_luma(const uint16_t* rgb565, int stride, int w, int h, uint8_t* out, int* lw, int* lh);

// The same, from a picture in the GPU's tiled layout, which is how the player's pooled
// pictures are stored: 8x8 tiles in rows from the top, Morton order inside each tile. tw is
// the tiled buffer's width, a multiple of 8.
void me_luma_tiled(const uint16_t* tiled, int tw, int w, int h, uint8_t* out, int* lw, int* lh);

// Finds the motion of every block from luma map a to luma map b (both lw x lh). prev is the
// previous pair's field, whose vectors seed the search, or NULL.
void me_estimate(const uint8_t* a, const uint8_t* b, int lw, int lh, const MotionField* prev, MotionField* f);

// Repeated frames. Many uploads are 24 or 25 fps films converted to 30 fps by showing every
// 4th or 5th frame twice; in a survey of 17 videos, 2 had over a hundred repeats a minute.
// Motion stops for a frame and then jumps, a stutter Smooth would faithfully copy. So we drop
// the repeat and let the in-between pictures span the frames around it.
// diff is the mean |a - b| between the luma maps of a frame and the last kept one. r holds
// the state; zero it at the start and after a seek. Returns true if the frame should be dropped.
typedef struct {
	float avg;
	bool last;
	int frame, last_small, rhythm, twos, twos_until;
} MeRepeat;
float me_diff(const uint8_t* a, const uint8_t* b, int lw, int lh);
bool me_repeat(float diff, MeRepeat* r);

// Re-timing around dropped repeats. In a 25 fps film shown at 30 fps, the frames are 33 ms
// apart on the file's clock but really 40 ms apart. Dropping the repeat leaves a single 67 ms
// gap, and the in-between pictures would crawl across it at half speed. So while repeats keep
// coming, kept frames are spaced evenly at the recent average spacing, and pulled gently
// towards the file's times. They never drift more than one spacing away, which keeps A/V sync
// within that.
// pts is the file's time in ms; repeated says a repeat was dropped just before this frame.
#define ME_RETIME_N 40
typedef struct {
	double t, last_pts, step, off, hist[ME_RETIME_N];
	int n, since_repeat;
	bool ok, active;
} MeRetime;
double me_retime(MeRetime* r, double pts, bool repeated);

// Motion at the mesh vertices, in picture pixels. The vertices are the block corners,
// (bw + 1) x (bh + 1) of them row by row, and each one averages the usable vectors of the
// blocks around it (0 if there are none).
void me_vertex_vectors(const MotionField* f, float* vx, float* vy);
