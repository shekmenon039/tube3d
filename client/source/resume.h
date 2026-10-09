#pragma once
// Remembers where each file in the Videos tab was left, along with the audio and subtitle
// tracks chosen, for "Continue watching". Only the RESUME_MAX most recent files are kept, in
// sdmc:/3ds/Tube3D/resume.txt.
#include <stdbool.h>
#include <stdint.h>

#define RESUME_MAX 64

typedef struct {
	uint32_t pos_ms;   // where playback stopped
	uint32_t dur_ms;   // the file's length, 0 if unknown
	int audio;         // the chosen audio stream (YtSrcTrack.stream); -1 for the file's default
	int sub;           // the chosen subtitle track (caption index); -1 off, -2 never chosen
} ResumeInfo;

// Looks up path. False if there's no entry for it.
bool resume_get(const char* path, ResumeInfo* out);

// Stores where path was left. Near the start or the end (it was watched through), the
// position isn't kept, so the file starts from the beginning next time. The entry itself
// goes too, unless audio or subtitle tracks were chosen near the start.
void resume_set(const char* path, const ResumeInfo* info);

// True when pos is worth resuming from: past the first 30 s, and not within the last minute
// or the last 5%.
bool resume_worth(uint32_t pos_ms, uint32_t dur_ms);
