#pragma once
// Fixed byte rings for stream packets (the player's video and audio queues).
#include <3ds.h>
#include <stdbool.h>
#include <string.h>

// Each stream gets one ring, allocated once per video in direct_play and reused across seeks
// and mode switches, so playback never mallocs per packet. There is exactly one producer (the
// reader thread) and one consumer (the main loop). Packets live in place: the producer
// reserves space, reads straight into it and commits; the consumer peeks, uses the packet and
// releases it.

typedef struct {
	u32 size;   // bytes this packet occupies in the ring, header included
	u8 type;
	u8 eye;   // 0 left, 1 right, 2 mono
	u16 pad;
	u32 pts;   // ms
	u32 len;
	u8 data[];
} Pkt;

#define RING_NO_MARK 0xFFFFFFFFu

typedef struct {
	u8* buf;
	u32 cap;
	u32 wpos, rpos;       // byte offsets
	u32 used;             // bytes in use, including a skipped tail after a wrap
	u32 end_mark;         // where the reader must wrap to 0, or RING_NO_MARK
	int count;            // committed packets
	volatile bool full;   // the producer's last reserve found no room. A big packet can need more
	                      // contiguous space than there is, so the reader has to drain some first.
	LightLock lock;
} Ring;

static inline void ring_init(Ring* r, u8* buf, u32 cap)
{
	memset(r, 0, sizeof(*r));
	r->buf = buf;
	r->cap = cap;
	r->end_mark = RING_NO_MARK;
	LightLock_Init(&r->lock);
}

static inline u32 pkt_total(u32 len) { return (sizeof(Pkt) + len + 1 + 3) & ~3u; }

// Finds room for a packet with len payload bytes, or returns NULL if the ring is too
// full. Data lives in [rpos, wpos), or after a wrap in [rpos, end_mark) plus [0, wpos).
static inline Pkt* ring_reserve(Ring* r, u32 len, bool* wraps)
{
	u32 total = pkt_total(len);
	bool ok = false;
	u32 pos = 0;
	*wraps = false;
	LightLock_Lock(&r->lock);
	if (r->used == 0)
	{
		r->wpos = r->rpos = 0;
		r->end_mark = RING_NO_MARK;
	}
	if (r->end_mark == RING_NO_MARK)
	{
		if (r->cap - r->wpos >= total)
		{
			pos = r->wpos;
			ok = true;
		}
		else if (r->rpos > total)
		{
			pos = 0;
			ok = true;
			*wraps = true;
		}
	}
	else if (r->rpos - r->wpos >= total)
	{
		pos = r->wpos;
		ok = true;
	}
	r->full = !ok;
	LightLock_Unlock(&r->lock);
	if (!ok) return NULL;
	Pkt* p = (Pkt*)(r->buf + pos);
	p->size = total;
	p->len = len;
	return p;
}

static inline void ring_commit(Ring* r, Pkt* p, bool wraps)
{
	LightLock_Lock(&r->lock);
	if (wraps)
	{
		r->end_mark = r->wpos;         // the reader jumps from here back to 0
		r->used += r->cap - r->wpos;   // the skipped tail counts as used until then
	}
	r->wpos = (u32)((u8*)p - r->buf) + p->size;
	r->used += p->size;
	r->count++;
	LightLock_Unlock(&r->lock);
}

static inline Pkt* ring_peek(Ring* r)
{
	Pkt* p = NULL;
	LightLock_Lock(&r->lock);
	if (r->count > 0)
	{
		if (r->rpos == r->end_mark)
		{
			r->used -= r->cap - r->end_mark;
			r->rpos = 0;
			r->end_mark = RING_NO_MARK;
		}
		p = (Pkt*)(r->buf + r->rpos);
	}
	LightLock_Unlock(&r->lock);
	return p;
}

static inline void ring_release(Ring* r)
{
	LightLock_Lock(&r->lock);
	Pkt* p = (Pkt*)(r->buf + r->rpos);
	r->rpos += p->size;
	r->used -= p->size;
	r->count--;
	LightLock_Unlock(&r->lock);
}

static inline int ring_count(Ring* r)
{
	LightLock_Lock(&r->lock);
	int n = r->count;
	LightLock_Unlock(&r->lock);
	return n;
}

static inline u32 ring_used(Ring* r)
{
	LightLock_Lock(&r->lock);
	u32 n = r->used;
	LightLock_Unlock(&r->lock);
	return n;
}
