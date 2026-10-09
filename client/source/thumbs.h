#pragma once
// Thumbnail cache. A background thread downloads YouTube's thumbnail JPEGs and turns them
// into tiled textures (one per eye for 3D videos); the main thread uploads them to the GPU.
#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>

void thumbs_init(void);
void thumbs_exit(void);

// Main thread. Frees every thumbnail's textures and pixels to make room in linear memory for
// a 1080p file's decoder. They load again the next time they're requested.
void thumbs_release(void);

// Ask for a thumbnail. layout is "2d", "sbs" or "tb". Higher priority loads first.
// Call every frame for the cards that are on screen.
void thumbs_request(const char* id, const char* layout, int priority);

// Main thread, once per frame. Moves downloaded pixels into GPU textures. Returns true while
// thumbnails are loading or were just uploaded, so the menu knows to redraw.
bool thumbs_update(void);

// Looks up a ready thumbnail. For 3D videos left/right are the two eyes;
// for 2D both point at the same image. avg_rgb is the picture's average colour.
// For 3D thumbnails, disparity is how far (in picture pixels, out of 224) the right eye's
// image is shifted against the left. The menu moves each eye by half of that to fuse them.
bool thumbs_get(const char* id, const char* layout, C2D_Image* left, C2D_Image* right, u32* avg_rgb, int* disparity);
