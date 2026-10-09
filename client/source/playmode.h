#pragma once
// How a video is shown: flat, or in one of the stereo layouts. The user picks it in Settings
// or with X during playback; Auto lets the player work it out from the title and the picture.

typedef enum {
	MODE_AUTO = 0,   // guess from the title, then let the picture decide (see direct_player.c)
	MODE_2D,
	MODE_SBS,        // side-by-side 3D (half or full width)
	MODE_TB,         // top-bottom 3D
	MODE_SBS_SWAP,   // side-by-side, right eye first (cross-eyed videos)
	MODE_TB_SWAP,
	MODE_COUNT
} PlayMode;

const char* play_mode_name(PlayMode m);   // human readable, e.g. "3D side-by-side"
