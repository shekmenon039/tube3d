#include "exitguard.h"
#include "tlog.h"
#include "yt.h"

#define WATCHDOG_MS 4000

static volatile bool requested = false;
static volatile bool finished = false;
static const char* volatile last_step = "start";

// Last resort when the normal shutdown gets stuck: close the way libctru does at the end of
// aptExit(), which ends the process and lets the HOME menu carry on. A plain exit() here
// would free the heaps while other threads are still using them.
static void watchdog(void* arg)
{
	for (int waited = 0; waited < WATCHDOG_MS && !finished; waited += 10) svcSleepThread(10 * 1000000ULL);
	if (finished) return;
	tlog("watchdog: shutdown stuck at '%s' for %d ms, closing through APT", last_step, WATCHDOG_MS);
	tlog_close();
	exit_through_apt();
}

void exit_through_apt(void)
{
	APT_PrepareToCloseApplication(true);
	APT_CloseApplication(NULL, 0, 0);
	svcExitProcess();
}

void exit_requested(const char* where)
{
	if (requested) return;
	requested = true;
	tlog("exit requested in %s%s", where, aptShouldClose() ? " (closed by the system: HOME menu or power)" : "");
	// threads blocked in the network return within about a second
	yt_abort();
	// High priority so it still runs if a busy thread hogs the core; detached, nobody joins it
	threadCreate(watchdog, NULL, 8 * 1024, 0x18, -2, true);
}

void exit_step(const char* step)
{
	last_step = step;
	tlog("exit: %s", step);
}

void exit_done(void)
{
	finished = true;
	tlog("exit finished");
	tlog_close();
}

// ---------------------------------------------------------------- HOME during playback

void playback_home_block(bool block)
{
	if (block) aptCheckHomePressRejected();   // forget any earlier press, e.g. one made during a message box
	aptSetHomeAllowed(!block);
}

bool playback_home_pressed(void) { return aptCheckHomePressRejected(); }

void playback_go_home(const char* where)
{
	tlog("HOME pressed during playback (%s): decoder stopped, jumping to HOME", where);
	aptSetHomeAllowed(true);
	aptJumpToHomeMenu();
	tlog("back from HOME (close requested: %d)", aptShouldClose());
	if (aptShouldClose()) exit_requested("HOME -> Close during playback");   // start the shutdown and its watchdog now
}
