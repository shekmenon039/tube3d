#pragma once
// The video player. ytsrc.c downloads and demuxes the stream, MVD decodes each frame into a
// linear buffer, the GPU tiles that into a texture, and ui.c (pui_video) draws each eye's part
// of the picture.
#include <3ds.h>
#include <stdbool.h>
#include "playmode.h"

// Plays a YouTube video by its 11-character id. The title is shown while loading and may be
// empty. mode is the playback mode from Settings, read and written back: in Auto the 3D layout
// is guessed from the title, and X cycles through layouts during playback. Pass audio = false
// if ndspInit() failed.
void direct_play(const char* video_id, const char* title, PlayMode* mode, bool audio);

// Sets the channel and view count shown under the title for the next direct_play, taken from
// the list the video was picked in. Pass "" when unknown; the channel then comes from
// YouTube's response.
void direct_set_meta(const char* channel, const char* views);

// The same player for a video file (H.264 in MKV or MP4), e.g. "sdmc:/...". The title is the
// file name, and words like "3D" or "SBS" in it hint at the layout just as a YouTube title does.
void direct_play_file(const char* path, const char* title, PlayMode* mode, bool audio);

// Sharing MVD with other code, such as the Videos tab's previews in filethumb.c. One user at a
// time; playback holds the lock from start to finish.
// direct_mvd_workbuf returns the work buffer size a w x h stream needs.
void direct_mvd_lock(void);
void direct_mvd_unlock(void);
u32 direct_mvd_workbuf(u32 w, u32 h, u8 level);
