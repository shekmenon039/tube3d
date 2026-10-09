#include "power.h"
#include "tlog.h"

#define POLL_INTERVAL_MS 5000   // how often power_poll() reads the battery again

// Service sessions are opened only for the moment they're needed. MCU services take one
// session at a time and make every other client wait (see 3dbrew): keeping mcu::HWC open for
// the battery percentage made the HOME menu wait forever, so pressing HOME hung the console.
// gsp::Lcd is treated the same way to be safe.
static bool ptm_ok = false;
static int percent = 100;
static bool charging = false;
static u64 last_poll = 0;
static bool backlight_on = true;
static aptHookCookie hook;

static Result set_backlight(bool on)
{
	Result r = gspLcdInit();
	if (R_FAILED(r)) return r;
	r = on ? GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM) : GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);
	gspLcdExit();
	return r;
}

static void restore_backlight(void)
{
	if (!backlight_on)
	{
		Result r = set_backlight(true);
		backlight_on = true;
		tlog("power: bottom backlight on (system event) -> %08lX", (unsigned long)r);
	}
}

// HOME menu, sleep and exit must never find the bottom screen dark
static void on_apt(APT_HookType type, void* param)
{
	if (type == APTHOOK_ONSUSPEND || type == APTHOOK_ONSLEEP || type == APTHOOK_ONEXIT) restore_backlight();
}

void power_init(void)
{
	ptm_ok = R_SUCCEEDED(ptmuInit());   // ptm:u takes many sessions (most apps keep one)
	aptHook(&hook, on_apt, NULL);
	last_poll = 0;
	power_poll();
}

void power_exit(void)
{
	aptUnhook(&hook);
	restore_backlight();
	if (ptm_ok) ptmuExit();
	ptm_ok = false;
}

// Exact battery percentage from the MCU, with the session closed again right away
static bool mcu_percent(u8* level)
{
	if (R_FAILED(mcuHwcInit())) return false;
	Result result = MCUHWC_GetBatteryLevel(level);
	mcuHwcExit();
	return R_SUCCEEDED(result) && *level <= 100;
}

void power_poll(void)
{
	u64 now = osGetTime();
	if (last_poll && now - last_poll < POLL_INTERVAL_MS) return;
	last_poll = now;
	u8 level = 0;
	if (mcu_percent(&level)) percent = level;
	else if (ptm_ok && R_SUCCEEDED(PTMU_GetBatteryLevel(&level)))
		percent = level >= 5 ? 100 : level * 20;   // 0-5 steps
	u8 state = 0;
	bool adapter = false;
	charging = ptm_ok && ((R_SUCCEEDED(PTMU_GetBatteryChargeState(&state)) && state == 1) ||
	                      (R_SUCCEEDED(PTMU_GetAdapterState(&adapter)) && adapter));
}

int power_percent(void) { return percent; }
bool power_charging(void) { return charging; }

void power_bottom_backlight(bool on)
{
	static bool lcd_failed = false;
	if (on == backlight_on || lcd_failed) return;
	Result r = set_backlight(on);
	if (R_FAILED(r))
	{
		lcd_failed = true;   // without gsp::Lcd the screen just stays lit, and we stop asking
		tlog("power: backlight control unavailable -> %08lX", (unsigned long)r);
		return;
	}
	backlight_on = on;
	tlog("power: bottom backlight %s -> %08lX", on ? "on" : "off", (unsigned long)r);
}

bool power_bottom_backlight_on(void) { return backlight_on; }
