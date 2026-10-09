#pragma once
// Makes closing the app fast and reliable, from START or from Close in the HOME menu. Call
// exit_requested() wherever aptMainLoop() returns false or the user quits. The first call
// logs where, aborts every curl transfer so threads stuck in network calls return, and starts
// a watchdog that ends the process if shutdown hasn't finished within a few seconds.
#include <3ds.h>
#include <stdbool.h>

void exit_requested(const char* where);

// Marks a shutdown step in the log (and keeps the watchdog informed).
void exit_step(const char* step);

// Called at the very end of main(): the shutdown finished normally.
void exit_done(void);

// Ends the process immediately, the same way libctru's aptExit() closes an application:
// prepare to close, close, then svcExitProcess. All threads stop together, so nothing keeps
// running after the heap is gone. Never returns.
void exit_through_apt(void) __attribute__((noreturn));

// ---- HOME button during playback
// While a video plays, the player blocks libctru's automatic jump to HOME. It notices the
// press with playback_home_pressed(), stops the decoder and audio cleanly, then goes to the
// HOME menu with playback_go_home(). That call returns when the user comes back, or when they
// chose Close (aptMainLoop() is false then). On return, playback carries on at the same
// position through an in-place seek, with the download still open.
// If HOME ever hangs, check that nothing holds an mcu:: session open (see power.c).
void playback_home_block(bool block);
bool playback_home_pressed(void);
void playback_go_home(const char* where);
