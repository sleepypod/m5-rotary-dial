#ifndef CONFIG_H
#define CONFIG_H

// WiFi Configuration - Stored in credentials.h (gitignored)
#include "credentials.h"

// Temperature Settings (internal unit: Fahrenheit, matching sleepypod-core API)
#define TEMP_MIN_F 55      // Minimum temperature (°F) - sleepypod-core hardware limit
#define TEMP_MAX_F 110     // Maximum temperature (°F) - sleepypod-core hardware limit
#define TEMP_DEFAULT_F 75  // Default temperature setpoint (°F)

// Sleepypod-core API Settings
#define POD_API_PORT 3000  // sleepypod-core tRPC HTTP port

// Display Settings
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 240
#define BRIGHTNESS_DAY 255   // 100% brightness during day
#define BRIGHTNESS_NIGHT 51  // 20% brightness at night (10pm-7am)
#define BRIGHTNESS_DIM 2     // ~1% brightness when inactive
#define DIM_TIMEOUT_MS 10000 // Dim after 10 seconds of inactivity
#define NIGHT_START_HOUR 22  // 10pm
#define NIGHT_END_HOUR 7     // 7am

// NTP Settings
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC -28800 // Pacific Standard Time (UTC-8)
#define DAYLIGHT_OFFSET_SEC 0 // Adjust for daylight saving time

// Day Mode Colors (RGB565 format)
#define COLOR_BACKGROUND 0x0000 // Black
#define COLOR_ARC_BG 0x2104     // Dark gray
#define COLOR_ARC_HOT 0xF800    // Red
#define COLOR_TEXT 0xFFFF       // White
#define COLOR_SETPOINT 0x07E0   // Green

// Night Mode Colors (RGB565 format - red theme)
#define COLOR_NIGHT_BACKGROUND 0x0000 // Black
#define COLOR_NIGHT_ARC_BG 0x2800     // Very dark red
#define COLOR_NIGHT_ARC_HOT 0xF800    // Bright red
#define COLOR_NIGHT_TEXT 0xF800       // Red text
#define COLOR_NIGHT_SETPOINT 0xC000   // Dark orange-red

#endif // CONFIG_H
