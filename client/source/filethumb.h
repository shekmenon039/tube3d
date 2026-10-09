#pragma once
// Previews for the Videos tab: one frame from inside a video file, decoded by the hardware
// decoder, plus what the container tells us (running time and picture size). This runs on
// the thumbnail thread (thumbs.c) and takes turns with the player on MVD (direct_mvd_lock).
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

// The thumbnail cache id for a file: "f" plus a hash of its path, since the cache keeps
// only short ids.
void filethumb_id(const char* path, char* id, size_t size);

// Remembers which file an id stands for, because thumbs.c asks for frames by id.
void filethumb_register(const char* id, const char* path);

// Decodes a frame from about a tenth of the way into the file, as RGB888 at most 320 px
// wide. *rgb is malloc'ed and the caller frees it. Thumbnail thread only; false on failure.
bool filethumb_frame(const char* id, u8** rgb, int* w, int* h);

// What the last frame grab of path found out: the duration in ms and the picture size.
// False if nothing has been grabbed yet.
bool filethumb_info(const char* path, u32* duration_ms, int* w, int* h);
