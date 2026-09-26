#ifndef CONFIG_H
#define CONFIG_H

// WiFi Configuration - Stored in credentials.h (gitignored)
#include "credentials.h"

// Temperature Settings (internal unit: Fahrenheit, matching sleepypod-core API)
#define TEMP_MIN_F 55      // Minimum temperature (°F) - sleepypod-core hardware limit
#define TEMP_MAX_F 110     // Maximum temperature (°F) - sleepypod-core hardware limit
#define TEMP_DEFAULT_F 75  // Default temperature setpoint (°F)

// sleepypod-core API Settings
#define POD_API_PORT 3000  // sleepypod-core tRPC HTTP port
#define HTTP_TIMEOUT_MS 1500 // Keep short: HTTP calls block the UI loop

// Display Settings
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 240
#define BRIGHTNESS_DAY 255   // 100% brightness during day
#define BRIGHTNESS_NIGHT 51  // 20% brightness at night (10pm-7am)
#define BRIGHTNESS_DIM 2     // ~1% brightness when inactive
#define DIM_TIMEOUT_MS 10000 // Dim after 10 seconds of inactivity (day)
#define DIM_TIMEOUT_NIGHT_MS 5000 // Screen glow is the enemy at night
#define SAFE_WAKE_ARM_MS 3000   // Input within 3s of dimming still acts; later it only wakes
#define WAKE_FADE_MS 250
#define SLEEP_FADE_MS 600
#define NIGHT_START_HOUR 22  // 10pm
#define NIGHT_END_HOUR 7     // 7am

// Interaction timing
#define SETTINGS_HOLD_MS 1500     // Hold the dial or screen to open settings (progress ring)
#define HOLD_RING_SHOW_MS 200     // Ring appears after this much of the hold
#define CLICK_MAX_MS 400          // Press shorter than this = click (toggle power)
#define ACCEL_WINDOW_MS 150       // 3 detents inside this window -> 2°F per detent
#define OFF_DETENTS 2             // Detents below minimum that reach the OFF stop
#define REMOTE_SYNC_HOLDOFF_MS 30000 // Don't let Pod sync overwrite a fresh local change

// Animation
#define ARC_SETTLE_MS 180
#define SIDE_SWITCH_MS 220

// NTP Settings
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC -28800 // Pacific Standard Time (UTC-8)
#define DAYLIGHT_OFFSET_SEC 0 // Adjust for daylight saving time

// Main-screen palette, day (RGB565)
#define UI_BG 0x0862         // #0B0E14 blue-black so the bezel reads as a ring
#define UI_TRACK 0x2125      // #1E232B unfilled arc
#define UI_MUTED 0x52EC      // #555C66 disabled numeral, hints
#define UI_SECONDARY 0x9D15  // #9AA0A8 unit, clock, inactive side
#define UI_TEXT 0xF79E       // #F2F2F0 numeral, active side
#define UI_ALERT 0xFAC7      // #FF5A3C offline / hot end of the arc
#define UI_COOL 0x361C       // #35C4E0 "cooling" status
#define UI_WARM 0xFD27       // #FFA53C "heating" status

// Main-screen palette, night (red only, tuned for 20% backlight)
#define UI_N_BG 0x0000
#define UI_N_LOW 0x3000      // #300000 track, hints
#define UI_N_MID 0x8800      // #880000 secondary
#define UI_N_HIGH 0xE000     // #E00000 numeral, active side, alerts

// Settings / secondary screens reuse the main-screen palette above
#define COLOR_BACKGROUND UI_BG
#define COLOR_ARC_BG UI_TRACK
#define COLOR_ARC_HOT UI_ALERT
#define COLOR_TEXT UI_SECONDARY
#define COLOR_SETPOINT UI_TEXT   // selected item / accent

#define COLOR_NIGHT_BACKGROUND UI_N_BG
#define COLOR_NIGHT_ARC_BG UI_N_LOW
#define COLOR_NIGHT_ARC_HOT UI_N_HIGH
#define COLOR_NIGHT_TEXT UI_N_MID
#define COLOR_NIGHT_SETPOINT UI_N_HIGH

#endif // CONFIG_H
