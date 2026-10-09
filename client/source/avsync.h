#pragma once
// The player's timing logic, kept free of 3DS calls so it also builds on a PC.
#include <stdbool.h>
#include <stdint.h>

// ---- smoothed media clock
// The audio position only advances once per DSP frame, in steps of about 5 ms, which is coarse
// enough to put frames a refresh early or late. So the clock runs on a fine time base (now_ms)
// and is nudged gently towards the raw position. A gap of more than 80 ms means something
// really happened (a seek, an underrun, a switch of clock), and then it snaps to the raw value.
typedef struct {
	double base, t0;
	bool ok;
} SmoothClock;

static inline void smooth_clock_reset(SmoothClock* s) { s->ok = false; }

static inline uint32_t smooth_clock(SmoothClock* s, uint32_t raw, double now_ms)
{
	double predicted = s->base + (now_ms - s->t0);
	double drift = (double)raw - predicted;
	if (!s->ok || drift > 80.0 || drift < -80.0)
	{
		s->base = raw;
		s->t0 = now_ms;
		s->ok = true;
		return raw;
	}
	s->base += drift * 0.02;
	return (uint32_t)(predicted + drift * 0.02);
}

// ---- how much to buffer before playing (re)starts
// After a start, a seek or an underrun the player waits for `full` ms of media. That's 2 s,
// plus 1.5 s for every underrun so far in the session (capped at 8 s), so a slow network
// settles down instead of stuttering forever. If the download has been filling the buffer at
// 1.5x real time or better, though, we start after only QUICK_BUFFER_MS: the buffer keeps
// growing while we play, and seeks on a good connection feel instant.
//
// The quick start is never used at the start of a session. The first response arrives as a
// burst (about 1 s of media in 0.2 s) that says nothing about the link, and on hardware some
// videos ran dry a few seconds after trusting it.
//
// queued_ms is the media queued since buffering began (the smaller of video and audio when
// both count); elapsed_ms is the time since data started arriving.
#define QUICK_BUFFER_MS 600u

static inline uint32_t buffer_want_ms(uint32_t queued_ms, uint32_t elapsed_ms, uint32_t underruns, bool session_start)
{
	uint32_t full = 2000u + 1500u * underruns;
	if (full > 8000u) full = 8000u;
	// Measure for at least 200 ms so a single burst doesn't pass for a fast link.
	if (!session_start && underruns == 0 && elapsed_ms >= 200u && queued_ms * 2u >= elapsed_ms * 3u)
		return QUICK_BUFFER_MS;
	return full;
}

// ---- leaving the buffering state
// We have enough once there are `prebuffer` video frames (or half the video ring is used) and,
// when audio drives the clock, `audio_prebuffer` audio packets (or half that ring). Reaching
// the end of the stream also counts. There's one more way out: if one ring is nearly full
// while the other is still short, the source can't add anything more, so waiting would never
// end. In that case we play what we have.
typedef struct {
	int video_count, audio_count;
	uint32_t video_used, video_cap, audio_used, audio_cap;
	int prebuffer;         // video packets wanted before playing
	int audio_prebuffer;   // audio packets wanted before playing (when need_audio)
	bool need_audio;       // audio drives the clock
	bool stream_done;      // eof or failed: nothing more is coming
	bool video_blocked;    // the source couldn't fit its next video packet (Ring.full)
} BufferState;

static inline bool buffering_done(const BufferState* b)
{
	bool video_ok = b->video_count >= b->prebuffer || b->video_used >= b->video_cap / 2 || b->stream_done;
	bool audio_ok =
	    !b->need_audio || b->audio_count >= b->audio_prebuffer || b->audio_used >= b->audio_cap / 2 || b->stream_done;
	// The rings can't take more, so start anyway. Note that a big access unit can fail to find
	// contiguous room even in a video ring that's only partly used.
	bool stuck = b->video_used >= b->video_cap / 8 * 7 || b->audio_used >= b->audio_cap / 8 * 7 ||
	             (b->video_blocked && b->video_count > 0);
	return (video_ok && audio_ok) || stuck;
}
