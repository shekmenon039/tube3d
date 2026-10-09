#include "wifi.h"
#include "tlog.h"
#include "yt.h"

#include <stdio.h>
#include <string.h>

#define POLL_INTERVAL_MS 1000   // how often wifi_poll() asks the system again

static bool ac_ok = false;
static bool online = false;
static u64 last_poll = 0;

bool wifi_init(void)
{
	ac_ok = R_SUCCEEDED(acInit());
	last_poll = 0;
	wifi_poll();
	return ac_ok;
}

void wifi_exit(void)
{
	if (ac_ok) acExit();
	ac_ok = false;
}

static bool query_online(void)
{
	u32 status = 0, ap_status = 0;
	const Result result = ac_ok ? ACU_GetStatus(&status) : -1;
	if (ac_ok) ACU_GetWifiStatus(&ap_status);
	static u32 last_status = 0xFFFFFFFF, last_ap_status = 0xFFFFFFFF;
	if (status != last_status || ap_status != last_ap_status)
	{
		tlog("wifi: status %lu (0x%08lX), access point %lu", (unsigned long)status, (unsigned long)result,
		     (unsigned long)ap_status);
		last_status = status;
		last_ap_status = ap_status;
	}
	// Only the low byte means "connected" (3). For about a second after the app starts, the
	// console answers 0x80003, which is connected plus a flag in the upper bits, so comparing
	// the whole value would report "No internet connection" at every start. A connected access
	// point (ap_status != 0) counts as online too.
	return ac_ok && R_SUCCEEDED(result) && ((status & 0xFF) == 3 || ap_status != 0);
}

void wifi_poll(void)
{
	u64 now = osGetTime();
	if (last_poll && now - last_poll < POLL_INTERVAL_MS) return;
	last_poll = now;
	online = query_online();
}

bool wifi_connected(void) { return online; }
u8 wifi_bars(void) { return online ? osGetWifiStrength() : 0; }

// ---------------------------------------------------------------- connection test

// One HTTPS request; true if the server answered with one of the expected statuses.
static bool timed_get(void* http, const char* url, u32* ms)
{
	static u8 body[4096];
	size_t len = 0;
	u64 start = osGetTime();
	int status = yt_http_get(http, url, body, sizeof(body), &len);
	*ms = (u32)(osGetTime() - start);
	return status == 204 || status == 200;
}

static void test_main(void* arg)
{
	WifiTest* t = (WifiTest*)arg;

	// 1. Wi-Fi link
	online = query_online();
	t->bars = online ? osGetWifiStrength() : 0;
	if (ac_ok)
	{
		char ssid[33] = { 0 };
		if (R_SUCCEEDED(ACU_GetSSID(ssid))) snprintf(t->ssid, sizeof(t->ssid), "%s", ssid);
	}
	t->link = online ? WT_OK : WT_FAIL;
	if (!online)
	{
		t->internet = t->youtube = WT_SKIP;
		t->done = true;
		return;
	}

	// 2. internet (Google's connectivity check) and 3. YouTube
	void* http = yt_http_new();
	t->internet = timed_get(http, "https://www.google.com/generate_204", &t->internet_ms) ? WT_OK : WT_FAIL;
	t->youtube = timed_get(http, "https://www.youtube.com/generate_204", &t->youtube_ms) ? WT_OK : WT_FAIL;
	yt_http_free(http);
	t->done = true;
}

bool wifi_test_start(WifiTest* t)
{
	memset(t, 0, sizeof(*t));
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// 48 KB: a TLS handshake runs on this thread
	t->thread = threadCreate(test_main, t, 48 * 1024, prio + 1, -2, false);
	if (!t->thread)
	{
		// No thread means no test. Report failure right away so the user can still leave the screen.
		t->link = t->internet = t->youtube = WT_FAIL;
		t->done = true;
	}
	return t->thread != NULL;
}

void wifi_test_end(WifiTest* t)
{
	if (t->thread)
	{
		threadJoin(t->thread, U64_MAX);
		threadFree(t->thread);
		t->thread = NULL;
	}
}

// ---------------------------------------------------------------- connect

bool wifi_connect(int timeout_ms)
{
	if (!ac_ok) return false;
	if (query_online()) return (online = true);
	acuConfig cfg;
	memset(&cfg, 0, sizeof(cfg));
	Handle event = 0;
	bool ok = R_SUCCEEDED(ACU_CreateDefaultConfig(&cfg)) && R_SUCCEEDED(ACU_SetNetworkArea(&cfg, 2)) &&
	          R_SUCCEEDED(ACU_SetAllowApType(&cfg, AC_AP_TYPE_SLOT1 | AC_AP_TYPE_SLOT2 | AC_AP_TYPE_SLOT3)) &&
	          R_SUCCEEDED(ACU_SetRequestEulaVersion(&cfg)) && R_SUCCEEDED(svcCreateEvent(&event, RESET_ONESHOT)) &&
	          R_SUCCEEDED(ACU_ConnectAsync(&cfg, event));
	// wait in short slices so closing the app (yt_abort) doesn't have to wait for it
	for (int waited = 0; ok && waited < timeout_ms && !yt_aborting(); waited += 100)
		if (svcWaitSynchronization(event, 100 * 1000000LL) == 0) break;
	if (event) svcCloseHandle(event);
	online = query_online();
	last_poll = osGetTime();
	return online;
}
