#include "motion.h"

#include <math.h>
#include <stdlib.h>

#define SEARCH_RANGE  24      // how far a block may move per frame, in luma-map pixels (96 picture pixels)
#define OK_SAD_PER_PX 14      // a match is trusted when the mean absolute difference (0-255) is below this
#define USABLE_SHARE  0.55f   // share of textured blocks that must match for a pair to be interpolated
#define LAMBDA        12      // SAD penalty per luma pixel of distance from the predicted vector
#define MIN_ACTIVITY  (ME_BLOCK * ME_BLOCK * 3)   // a block with at least this much activity counts as detailed

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static inline int med3i(int a, int b, int c)
{
	if (a > b)
	{
		int t = a;
		a = b;
		b = t;
	}
	if (b > c) b = c;
	return a > b ? a : b;
}

// How much detail a block of the luma map has: the summed differences between each pixel and
// its right and lower neighbours
static int activity(const uint8_t* a, int lw, int ax, int ay)
{
	int sum = 0;
	for (int y = 0; y < ME_BLOCK - 1; y++)
	{
		const uint8_t* row = a + (ay + y) * lw + ax;
		for (int x = 0; x < ME_BLOCK - 1; x++) sum += abs(row[x] - row[x + 1]) + abs(row[x] - row[x + lw]);
	}
	return sum;
}

static inline unsigned luma565(unsigned pixel)
{
	// Expand 5-6-5 to 8 bits per channel. Whether red or blue comes first doesn't matter here.
	unsigned c0 = (pixel >> 11) << 3, c1 = ((pixel >> 5) & 63) << 2, c2 = (pixel & 31) << 3;
	return c0 * 77 + c1 * 150 + c2 * 29;
}

void me_luma(const uint16_t* px, int stride, int w, int h, uint8_t* out, int* lw, int* lh)
{
	int out_w = w / ME_SCALE, out_h = h / ME_SCALE;
	if (out_w > ME_MAX_W) out_w = ME_MAX_W;
	if (out_h > ME_MAX_H) out_h = ME_MAX_H;
	// Sample each ME_SCALE x ME_SCALE cell at the centres of its four quarters. That's a quarter
	// of the reads of a full average, which matters on the 3DS CPU, and still smooth enough for
	// matching.
	const int near = ME_SCALE / 4, far = ME_SCALE * 3 / 4;
	for (int y = 0; y < out_h; y++)
	{
		const uint16_t* row_near = px + (y * ME_SCALE + near) * stride;
		const uint16_t* row_far = px + (y * ME_SCALE + far) * stride;
		for (int x = 0; x < out_w; x++)
		{
			int cell_x = x * ME_SCALE;
			unsigned samples[4] = { row_near[cell_x + near], row_near[cell_x + far], row_far[cell_x + near],
				                    row_far[cell_x + far] };
			unsigned sum = 0;
			for (int i = 0; i < 4; i++) sum += luma565(samples[i]);
			out[y * out_w + x] = (uint8_t)(sum / (4 * 256));
		}
	}
	*lw = out_w;
	*lh = out_h;
}

static inline unsigned tile_index(unsigned x, unsigned y, unsigned tiles_per_row)
{
	unsigned morton = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
	return ((y >> 3) * tiles_per_row + (x >> 3)) * 64 + morton;
}

void me_luma_tiled(const uint16_t* tiled, int tw, int w, int h, uint8_t* out, int* lw, int* lh)
{
	int out_w = w / ME_SCALE, out_h = h / ME_SCALE;
	if (out_w > ME_MAX_W) out_w = ME_MAX_W;
	if (out_h > ME_MAX_H) out_h = ME_MAX_H;
	const unsigned tiles_per_row = (unsigned)tw / 8;
	const int near = ME_SCALE / 4, far = ME_SCALE * 3 / 4;   // quarter centres, as in me_luma
	for (int y = 0; y < out_h; y++)
	{
		unsigned y0 = (unsigned)(y * ME_SCALE + near), y1 = (unsigned)(y * ME_SCALE + far);
		for (int x = 0; x < out_w; x++)
		{
			unsigned x0 = (unsigned)(x * ME_SCALE + near), x1 = (unsigned)(x * ME_SCALE + far);
			unsigned sum =
			    luma565(tiled[tile_index(x0, y0, tiles_per_row)]) + luma565(tiled[tile_index(x1, y0, tiles_per_row)]) +
			    luma565(tiled[tile_index(x0, y1, tiles_per_row)]) + luma565(tiled[tile_index(x1, y1, tiles_per_row)]);
			out[y * out_w + x] = (uint8_t)(sum / (4 * 256));
		}
	}
	*lw = out_w;
	*lh = out_h;
}

// SAD of the block at (ax, ay) in a against b shifted by (dx, dy). Gives up early once the
// sum reaches limit.
static int sad8(const uint8_t* a, const uint8_t* b, int lw, int lh, int ax, int ay, int dx, int dy, int limit)
{
	int sum = 0;
	int b_x0 = ax + dx;
	bool inside_x = b_x0 >= 0 && b_x0 + ME_BLOCK <= lw;
	for (int y = 0; y < ME_BLOCK; y++)
	{
		const uint8_t* row_a = a + (ay + y) * lw + ax;
		const uint8_t* row_b = b + clampi(ay + y + dy, 0, lh - 1) * lw;
		if (inside_x)
			for (int x = 0; x < ME_BLOCK; x++) sum += abs(row_a[x] - row_b[b_x0 + x]);
		else
			for (int x = 0; x < ME_BLOCK; x++) sum += abs(row_a[x] - row_b[clampi(b_x0 + x, 0, lw - 1)]);
		if (sum >= limit) return sum;
	}
	return sum;
}

// Sub-pixel offset, within +-0.5, from the costs at -1, 0 and +1. SAD grows roughly linearly
// away from the true minimum, so we fit a V rather than a parabola. A parabola pulls offsets
// towards whole pixels: on a known pan it was off by 0.2-0.6 px, nearly half of a slow
// 1 px/frame pan, and slow pans wobbled. The V fit gets the global motion about 3x closer
// (measured with a PC simulation on real videos).
static float subpel(int cost_minus, int cost_mid, int cost_plus)
{
	int hi = cost_minus > cost_plus ? cost_minus : cost_plus;
	if (hi - cost_mid <= 0) return 0.0f;
	float offset = (float)(cost_minus - cost_plus) / (2.0f * (float)(hi - cost_mid));
	return offset > 0.5f ? 0.5f : offset < -0.5f ? -0.5f : offset;
}

// Median of up to 9 values, sorting them in place
static float median(float* v, int n)
{
	for (int i = 1; i < n; i++)
	{
		float x = v[i];
		int j = i - 1;
		while (j >= 0 && v[j] > x)
		{
			v[j + 1] = v[j];
			j--;
		}
		v[j + 1] = x;
	}
	return n & 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) * 0.5f;
}

// Median of n values by quickselect, which reorders them
static float select_median(float* v, int n)
{
	int lo = 0, hi = n - 1, k = n / 2;
	while (lo < hi)
	{
		float pivot = v[(lo + hi) / 2];
		int i = lo, j = hi;
		while (i <= j)
		{
			while (v[i] < pivot) i++;
			while (v[j] > pivot) j--;
			if (i <= j)
			{
				float t = v[i];
				v[i] = v[j];
				v[j] = t;
				i++;
				j--;
			}
		}
		if (k <= j) hi = j;
		else if (k >= i) lo = i;
		else break;
	}
	return v[k];
}

void me_estimate(const uint8_t* a, const uint8_t* b, int lw, int lh, const MotionField* prev, MotionField* f)
{
	int bw = lw / ME_BLOCK, bh = lh / ME_BLOCK;
	if (bw > ME_MAX_BW) bw = ME_MAX_BW;
	if (bh > ME_MAX_BH) bh = ME_MAX_BH;
	f->bw = bw;
	f->bh = bh;
	f->good = 0;
	int textured = 0, mismatched = 0;   // blocks with detail, and how many of those found no good match
	static int whole_x[ME_MAX_BH][ME_MAX_BW],
	    whole_y[ME_MAX_BH][ME_MAX_BW];   // whole-pixel vectors, used as neighbour candidates
	const bool use_prev = prev && prev->bw == bw && prev->bh == bh;
	for (int by = 0; by < bh; by++)
		for (int bx = 0; bx < bw; bx++)
		{
			int ax = bx * ME_BLOCK, ay = by * ME_BLOCK;
			// Candidates: no motion, the neighbours already done, this block's vector last time,
			// and last time's global motion (MVTools' "global" predictor, which catches pans).
			int cand[6][2] = { { 0, 0 } };
			int cand_count = 1;
			if (bx > 0)
			{
				cand[cand_count][0] = whole_x[by][bx - 1];
				cand[cand_count][1] = whole_y[by][bx - 1];
				cand_count++;
			}
			if (by > 0)
			{
				cand[cand_count][0] = whole_x[by - 1][bx];
				cand[cand_count][1] = whole_y[by - 1][bx];
				cand_count++;
			}
			if (by > 0 && bx + 1 < bw)
			{
				cand[cand_count][0] = whole_x[by - 1][bx + 1];
				cand[cand_count][1] = whole_y[by - 1][bx + 1];
				cand_count++;
			}
			if (use_prev)
			{
				cand[cand_count][0] = (int)(prev->vx[by][bx] + (prev->vx[by][bx] >= 0 ? 0.5f : -0.5f));
				cand[cand_count][1] = (int)(prev->vy[by][bx] + (prev->vy[by][bx] >= 0 ? 0.5f : -0.5f));
				cand_count++;
				cand[cand_count][0] = (int)(prev->gx + (prev->gx >= 0 ? 0.5f : -0.5f));
				cand[cand_count][1] = (int)(prev->gy + (prev->gy >= 0 ? 0.5f : -0.5f));
				cand_count++;
			}
			// The predicted vector is the median of the neighbours (or zero). Straying from it
			// costs a little, so flat or repetitive areas, where many shifts match about equally
			// well, follow their surroundings instead of jumping to a random one.
			int pred_x = 0, pred_y = 0;
			if (cand_count >= 4)
			{
				pred_x = med3i(cand[1][0], cand[2][0], cand[3][0]);
				pred_y = med3i(cand[1][1], cand[2][1], cand[3][1]);
			}
			else if (cand_count >= 2)
			{
				pred_x = cand[1][0];
				pred_y = cand[1][1];
			}
			int best_x = 0, best_y = 0, best = 1 << 30, best_sad = 1 << 30;
			for (int i = 0; i < cand_count; i++)
			{
				int dx = clampi(cand[i][0], -SEARCH_RANGE, SEARCH_RANGE);
				int dy = clampi(cand[i][1], -SEARCH_RANGE, SEARCH_RANGE);
				int penalty = LAMBDA * (abs(dx - pred_x) + abs(dy - pred_y));
				int sad = sad8(a, b, lw, lh, ax, ay, dx, dy, best - penalty);
				if (sad + penalty < best)
				{
					best = sad + penalty;
					best_sad = sad;
					best_x = dx;
					best_y = dy;
				}   // ties keep the earlier one
			}
			// Diamond search, first in steps of 2, then 1
			for (int step = 2; step >= 1; step--)
				for (int iter = 0; iter < 12; iter++)
				{
					static const int dirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
					int next_x = best_x, next_y = best_y;
					for (int dir = 0; dir < 4; dir++)
					{
						int dx = best_x + dirs[dir][0] * step, dy = best_y + dirs[dir][1] * step;
						if (dx < -SEARCH_RANGE || dx > SEARCH_RANGE || dy < -SEARCH_RANGE || dy > SEARCH_RANGE)
							continue;
						int penalty = LAMBDA * (abs(dx - pred_x) + abs(dy - pred_y));
						int sad = sad8(a, b, lw, lh, ax, ay, dx, dy, best - penalty);
						if (sad + penalty < best)
						{
							best = sad + penalty;
							best_sad = sad;
							next_x = dx;
							next_y = dy;
						}
					}
					if (next_x == best_x && next_y == best_y) break;
					best_x = next_x;
					best_y = next_y;
				}
			whole_x[by][bx] = best_x;
			whole_y[by][bx] = best_y;
			best = best_sad;
			// Refine to sub-pixel precision from the costs around the minimum
			const int big = 1 << 30;
			float sub_x = subpel(sad8(a, b, lw, lh, ax, ay, best_x - 1, best_y, big), best,
			                     sad8(a, b, lw, lh, ax, ay, best_x + 1, best_y, big));
			float sub_y = subpel(sad8(a, b, lw, lh, ax, ay, best_x, best_y - 1, big), best,
			                     sad8(a, b, lw, lh, ax, ay, best_x, best_y + 1, big));
			f->vx[by][bx] = (float)best_x + sub_x;
			f->vy[by][bx] = (float)best_y + sub_y;
			// Trust a vector only for a close match on a block with real detail. Flat or black
			// blocks like sky or letterbox bars match almost anywhere, so they're handled below.
			bool detail = activity(a, lw, ax, ay) >= MIN_ACTIVITY;
			bool match = best < OK_SAD_PER_PX * ME_BLOCK * ME_BLOCK;
			f->ok[by][bx] = detail && match;
			textured += detail;
			mismatched += detail && !match;
			f->good += f->ok[by][bx];
		}

	// A 3x3 median over the trusted vectors removes isolated wrong matches
	static float med_x[ME_MAX_BH][ME_MAX_BW], med_y[ME_MAX_BH][ME_MAX_BW];
	for (int by = 0; by < bh; by++)
		for (int bx = 0; bx < bw; bx++)
		{
			float xs[9], ys[9];
			int count = 0;
			for (int y = by - 1; y <= by + 1; y++)
				for (int x = bx - 1; x <= bx + 1; x++)
					if (y >= 0 && y < bh && x >= 0 && x < bw && f->ok[y][x])
					{
						xs[count] = f->vx[y][x];
						ys[count] = f->vy[y][x];
						count++;
					}
			if (count >= 3 && f->ok[by][bx])
			{
				med_x[by][bx] = median(xs, count);
				med_y[by][bx] = median(ys, count);
			}
			else
			{
				med_x[by][bx] = f->vx[by][bx];
				med_y[by][bx] = f->vy[by][bx];
			}
		}
	// Global motion is the median of all trusted vectors
	static float gxs[ME_MAX_BH * ME_MAX_BW], gys[ME_MAX_BH * ME_MAX_BW];
	int global_count = 0;
	for (int by = 0; by < bh; by++)
		for (int bx = 0; bx < bw; bx++)
			if (f->ok[by][bx])
			{
				gxs[global_count] = med_x[by][bx];
				gys[global_count] = med_y[by][bx];
				global_count++;
			}
	f->gx = global_count ? select_median(gxs, global_count) : 0.0f;
	f->gy = global_count ? select_median(gys, global_count) : 0.0f;

	for (int by = 0; by < bh; by++)
		for (int bx = 0; bx < bw; bx++)
		{
			float vx = med_x[by][bx], vy = med_y[by][bx];
			if (f->ok[by][bx] && use_prev && prev->ok[by][bx] && fabsf(vx - prev->vx[by][bx]) < 1.0f &&
			    fabsf(vy - prev->vy[by][bx]) < 1.0f)
			{
				// Nearly the same motion as last time, so average the two. This keeps sub-pixel
				// noise from making the mesh wobble between pairs (a cheap version of MVTools'
				// temporal coherence).
				vx = (vx + prev->vx[by][bx]) * 0.5f;
				vy = (vy + prev->vy[by][bx]) * 0.5f;
			}
			f->vx[by][bx] = vx;
			f->vy[by][bx] = vy;
			if (!f->ok[by][bx] && activity(a, lw, bx * ME_BLOCK, by * ME_BLOCK) < MIN_ACTIVITY)
			{
				// Flat areas (sky, walls, bars) move with the whole picture, so a pan carries them along
				f->vx[by][bx] = f->gx;
				f->vy[by][bx] = f->gy;
				f->ok[by][bx] = 1;
			}
		}
	f->quality = textured ? 1.0f - (float)mismatched / (float)textured : 1.0f;
	// If many detailed blocks found no match, this is a cut or motion too fast to follow
	f->usable = bw * bh > 0 && (float)mismatched <= (1.0f - USABLE_SHARE) * (float)textured;
}

float me_diff(const uint8_t* a, const uint8_t* b, int lw, int lh)
{
	uint32_t sum = 0;
	for (int i = 0; i < lw * lh; i++) sum += (uint32_t)abs(a[i] - b[i]);
	return lw > 0 && lh > 0 ? (float)sum / (float)(lw * lh) : 0.0f;
}

bool me_repeat(float diff, MeRepeat* r)
{
	// A repeat is the same picture again, differing only by encoder noise. In a 24-to-30 fps
	// upload that noise is 0.00-0.05 on the luma map, while real frames differ by 0.1-1.3 in
	// slow scenes and 3-12 in fast ones. So a frame counts as a repeat when it's practically
	// identical, well under the recent change, in a scene that moves at all, and never twice in
	// a row (a scene that really stops is still shown). Dark slow scenes change little but
	// steadily (0.2-0.7) and must stay.
	// A purely relative rule caught under half the repeats of a slow upload, so dropped and
	// kept repeats alternated, which stuttered worse than dropping none.
	// In fast scenes the encoder doesn't copy a repeat exactly (0.05-0.35 against a normal
	// 0.9-1.9). So a frame changing less than 35% of the recent mean also counts, but only
	// when it keeps the pulldown's rhythm, the last such frame being 4-6 frames back. The
	// scattered look-alikes in dark slow scenes don't keep that rhythm.
	r->frame++;
	bool exact = diff < 0.08f && diff < 0.3f * r->avg;
	bool small = diff < 0.35f * r->avg;
	if (small)
	{
		int gap = r->frame - r->last_small;
		// Animation drawn on twos holds every other frame, and dropping those holds looked
		// worse. So look-alikes in a 2-frame rhythm pause the dropping for a while.
		r->twos = gap == 2 ? r->twos + 1 : 0;
		if (r->twos >= 2) r->twos_until = r->frame + 30;
		r->rhythm = gap >= 4 && gap <= 6 ? r->rhythm + 1 : 0;
		r->last_small = r->frame;
	}
	bool repeat = !r->last && r->frame >= r->twos_until && r->avg > 0.1f && (exact || (small && r->rhythm >= 1));
	if (!repeat)
	{
		float capped = diff > 2.0f * r->avg + 2.0f ? 2.0f * r->avg + 2.0f : diff;   // cuts don't inflate it
		r->avg = r->avg > 0.0f ? r->avg * 0.8f + capped * 0.2f : capped;
	}
	r->last = repeat;
	return repeat;
}

double me_retime(MeRetime* r, double pts, bool repeated)
{
	if (!r->ok || pts < r->last_pts || pts - r->last_pts > 200.0)
	{
		// At the start, after a seek or across a gap, just use the file's time
		r->ok = true;
		r->t = r->last_pts = pts;
		r->step = r->off = 0;
		r->hist[0] = pts;
		r->n = 1;
		r->since_repeat = 1000;
		r->active = false;
		return pts;
	}
	r->last_pts = pts;
	r->hist[r->n % ME_RETIME_N] = pts;
	r->n++;
	r->since_repeat = repeated ? 0 : r->since_repeat + 1;
	int window = r->n < ME_RETIME_N ? r->n : ME_RETIME_N;
	bool was_active = r->active;
	r->active = r->since_repeat <= 60 && window >= 8;
	if (!r->active || !was_active)
	{
		r->t = pts;   // no recent repeats (or starting over), so the file's times are already even
		r->off = 0;
	}
	else
	{
		// In reality the kept frames of a pulldown are evenly spaced. So each one is placed the
		// mean spacing of the last n frames after the previous one; how many repeats fall in
		// the window changes that by under a millisecond. The clock is then drawn very slowly
		// towards the file's times through its smoothed offset from them. The file's times are
		// naturally uneven by up to a frame, and following them directly made the motion speed
		// swing between 33 and 42 ms per frame.
		r->step = (pts - r->hist[(r->n - window) % ME_RETIME_N]) / (window - 1);
		double next = r->t + r->step;
		r->off = r->off * 0.95 + (pts - next) * 0.05;
		next += r->off * 0.02;
		if (next > pts + r->step) next = pts + r->step;
		if (next < pts - r->step) next = pts - r->step;
		if (next <= r->t) next = r->t + 1.0;
		r->t = next;
	}
	return r->t;
}

void me_vertex_vectors(const MotionField* f, float* vx, float* vy)
{
	int vw = f->bw + 1, vh = f->bh + 1;
	for (int y = 0; y < vh; y++)
		for (int x = 0; x < vw; x++)
		{
			float sum_x = 0, sum_y = 0;
			int count = 0;
			for (int by = y - 1; by <= y; by++)
				for (int bx = x - 1; bx <= x; bx++)
					if (by >= 0 && by < f->bh && bx >= 0 && bx < f->bw && f->ok[by][bx])
					{
						sum_x += f->vx[by][bx];
						sum_y += f->vy[by][bx];
						count++;
					}
			float off_x = count ? sum_x / (float)count * ME_SCALE : 0.0f,
			      off_y = count ? sum_y / (float)count * ME_SCALE : 0.0f;
			// Pin the edge vertices so the mesh never pulls away and leaves black gaps
			if (x == 0 || x == vw - 1) off_x = 0.0f;
			if (y == 0 || y == vh - 1) off_y = 0.0f;
			vx[y * vw + x] = off_x;
			vy[y * vw + x] = off_y;
		}
}
