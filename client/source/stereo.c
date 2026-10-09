#include "stereo.h"

uint32_t stereo_sample_eyes(const uint16_t* px, uint32_t stride, uint32_t iw, uint32_t ih, bool tb, bool swap,
                            uint8_t* left, uint8_t* right, uint32_t ow, uint32_t oh)
{
	uint32_t eye_w = tb ? iw : iw / 2, eye_h = tb ? ih / 2 : ih;
	uint32_t block_w = eye_w / ow ? eye_w / ow : 1, block_h = eye_h / oh ? eye_h / oh : 1;
	for (int eye = 0; eye < 2; eye++)
	{
		int half = swap ? eye ^ 1 : eye;
		uint32_t eye_x = !tb && half ? eye_w : 0, eye_y = tb && half ? eye_h : 0;
		uint8_t* dst = eye ? right : left;
		for (uint32_t y = 0; y < oh; y++)
		{
			const uint16_t* row = px + (eye_y + y * eye_h / oh) * stride + eye_x;
			for (uint32_t x = 0; x < ow; x++)
			{
				const uint16_t* block = row + x * eye_w / ow;
				uint32_t sum = 0;
				for (uint32_t j = 0; j < block_h; j++)
					for (uint32_t i = 0; i < block_w; i++) sum += stereo_luma565(block[j * stride + i]);
				*dst++ = (uint8_t)(sum / (block_w * block_h));
			}
		}
	}
	return eye_w;
}

bool stereo_textured(const uint8_t* img, int w, int h)
{
	uint32_t sum = 0, count = 0, dev = 0;
	for (int y = h / 6; y < h - h / 6; y++)
		for (int x = 0; x < w; x++)
		{
			sum += img[y * w + x];
			count++;
		}
	if (!count) return false;
	int mean = (int)(sum / count);
	for (int y = h / 6; y < h - h / 6; y++)
		for (int x = 0; x < w; x++)
		{
			int v = (int)img[y * w + x] - mean;
			dev += (uint32_t)(v < 0 ? -v : v);
		}
	return dev / count >= 10;   // mean distance from the average, in luminance steps
}

int stereo_analyse(const uint8_t* left, const uint8_t* right, int w, int h, int max_d, bool* pair)
{
	if (pair) *pair = true;   // too small to tell: trust the caller's guess
	if (w <= 2 * max_d + 4 || h < 6) return 0;
	uint32_t best = UINT32_MAX;
	int best_shift = 0;
	for (int shift = -max_d; shift <= max_d; shift++)
	{
		uint32_t sad = 0, count = 0;
		for (int y = h / 6; y < h - h / 6; y++)
		{
			const uint8_t* l = left + y * w;
			const uint8_t* r = right + y * w;
			for (int x = max_d; x < w - max_d; x++)
			{
				int diff = (int)l[x] - (int)r[x + shift];
				sad += (uint32_t)(diff < 0 ? -diff : diff);
			}
			count += (uint32_t)(w - 2 * max_d);
		}
		// mean difference in 1/64 steps, plus a small bias towards small shifts
		uint32_t score = (uint32_t)((uint64_t)sad * 64 / (count ? count : 1)) + (uint32_t)(shift < 0 ? -shift : shift);
		if (score < best)
		{
			best = score;
			best_shift = shift;
		}
	}
	if (pair)
	{
		// Compare the best match against the left picture's contrast (how far its pixels sit
		// from their average, on average). Two eyes of one scene differ by only a small part of
		// that at the best shift, while two unrelated pictures, such as the halves of an
		// ordinary 2D frame, differ by about the contrast itself. This tells side-by-side from
		// 2D correctly on real YouTube videos (tested on a PC), but macro 3D with very
		// uneven depth can fail it, so callers should treat "not a pair" as weak evidence.
		// A block-by-block version was tried and found false pairs in dark 2D frames.
		uint32_t sum = 0, count = 0, dev = 0;
		for (int y = h / 6; y < h - h / 6; y++)
			for (int x = max_d; x < w - max_d; x++)
			{
				sum += left[y * w + x];
				count++;
			}
		int mean = (int)(sum / (count ? count : 1));
		for (int y = h / 6; y < h - h / 6; y++)
			for (int x = max_d; x < w - max_d; x++)
			{
				int v = (int)left[y * w + x] - mean;
				dev += (uint32_t)(v < 0 ? -v : v);
			}
		uint32_t contrast = (uint32_t)((uint64_t)dev * 64 / (count ? count : 1));
		uint32_t diff = best - (uint32_t)(best_shift < 0 ? -best_shift : best_shift);
		uint32_t limit = contrast / 2 > 6 * 64 ? contrast / 2 : 6 * 64;
		*pair = diff < limit;
	}
	return best_shift;
}
