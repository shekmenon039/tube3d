#pragma once
// Draws the video picture as a textured mesh whose vertices are each moved by a motion vector,
// for the Smooth motion setting (see motion.h). It's plain citro3d with its own shader
// (vmesh.v.pica). Call it inside a citro2d scene: it flushes citro2d's batch first and
// restores citro2d's GPU state afterwards, so overlays drawn with citro2d still work.
#include <citro3d.h>
#include <stdbool.h>

typedef struct {
	C3D_Tex* tex;
	float tex_w, tex_h;   // texture size; the picture starts at its top-left (v = 1 is the top row)
	float org_x, org_y;   // where the mesh starts in the picture (one eye of a 3D layout: its half)
	float pic_w, pic_h;   // the mesh's size in picture pixels (the eye's half, or the whole picture)
	int vw, vh;           // mesh vertices across and down (blocks + 1)
	float cell;           // vertex spacing in picture pixels (the last row/column sits on the edge)
	const float* vx;      // vertex offsets in picture pixels, vw * vh, row by row
	const float* vy;
	float s;   // scale for the offsets: t for picture A, -(1 - t) for picture B
	// Maps picture to screen as screen = (picture - src) * scale + dst, like video_eye does
	float src_x, src_y, scale_x, scale_y, dst_x, dst_y;
	float alpha;   // opacity (1 for the first mesh, t for the second)
} VMeshDraw;

bool vmesh_init(void);   // false if Smooth can't be drawn; plain frames are shown instead
void vmesh_exit(void);
bool vmesh_ready(void);
void vmesh_draw(const VMeshDraw* d);
static inline void vmesh_frame_reset(void) {}
