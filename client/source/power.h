#pragma once
// Battery readings, bottom-screen backlight and CPU speed.
#include <3ds.h>
#include <stdbool.h>

void power_init(void);
void power_exit(void);   // restores the backlight and the CPU speed

// Refreshes the battery state (cheap: queries the hardware every few seconds).
void power_poll(void);
int power_percent(void);     // 0-100 (from the 0-5 level if the exact value isn't available)
bool power_charging(void);   // charger plugged in and charging (or full on the charger)

// Bottom-screen backlight. It is switched back on automatically whenever the app is
// suspended (HOME menu), goes to sleep or exits.
void power_bottom_backlight(bool on);
bool power_bottom_backlight_on(void);

// We deliberately don't slow the CPU down when idle. Switching the New 3DS between 804 MHz
// with L2 cache and 268 MHz at run time (osSetSpeedupEnable) made every later jump to the
// HOME menu hang on a real console. The CPU stays at the RSF's 804 MHz + L2 the whole time.
