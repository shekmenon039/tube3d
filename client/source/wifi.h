#pragma once
// Wi-Fi status for the header icon, a step-by-step connection test for Settings, and
// asking the system to connect with the networks saved in System Settings.
#include <3ds.h>
#include <stdbool.h>

// Step results: pending while the test runs, then ok or failed (or skipped).
typedef enum { WT_PENDING, WT_OK, WT_FAIL, WT_SKIP } WifiStep;

typedef struct {
	volatile bool done;
	WifiStep link, internet, youtube;
	u8 bars;   // 0-3 signal strength
	char ssid[33];
	u32 internet_ms, youtube_ms;
	Thread thread;
} WifiTest;

bool wifi_init(void);
void wifi_exit(void);

// Cheap to call every frame: refreshes the connection state about once a second.
void wifi_poll(void);
bool wifi_connected(void);   // the console has an internet connection
u8 wifi_bars(void);          // 0-3, as in the HOME menu

// Runs the test on a background thread; poll t->done, then wifi_test_end().
bool wifi_test_start(WifiTest* t);
void wifi_test_end(WifiTest* t);

// Asks the system to connect using the saved networks (blocks up to timeout_ms).
// Returns true once the console is online.
bool wifi_connect(int timeout_ms);
