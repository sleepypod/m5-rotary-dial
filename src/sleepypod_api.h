#ifndef SLEEPYPOD_API_H
#define SLEEPYPOD_API_H

#include <Arduino.h>
#include <IPAddress.h>

// Pod status for a single side
struct SideStatus
{
  int targetTemperatureF; // 55-110
  int currentTemperatureF;
  bool isPowered;
  bool valid; // true if successfully parsed
};

// Full pod status
struct PodStatus
{
  SideStatus left;
  SideStatus right;
  bool success; // true if API call succeeded
};

// Discover the Pod on the local network via mDNS (_sleepypod._tcp)
// Returns true if found, sets podIP and podPort
bool discoverPod(IPAddress &podIP, uint16_t &podPort);

// Fetch device status from sleepypod-core
// GET /api/device/status
PodStatus fetchPodStatus(IPAddress ip, uint16_t port = 3000);

// Set temperature for a side
// POST /api/device/temperature
// side: "left" or "right", temperature: 55-110°F
bool setPodTemperature(IPAddress ip, const char *side, int temperatureF, uint16_t port = 3000);

// Set power state for a side
// POST /api/device/power
// side: "left" or "right", powered: true/false
bool setPodPower(IPAddress ip, const char *side, bool powered, uint16_t port = 3000);

// Pod settings (from GET /api/settings)
struct PodSettings
{
  String leftName;       // Display name for left side (e.g., "Nick")
  String rightName;      // Display name for right side (e.g., "Partner")
  String temperatureUnit; // "F" or "C"
  bool rebootDaily;      // Whether Pod reboots daily
  String rebootTime;     // HH:mm format
  bool success;
};

// Fetch settings from sleepypod-core
// GET /api/settings
PodSettings fetchPodSettings(IPAddress ip, uint16_t port = 3000);

// Temperature conversion utilities
float fahrenheitToCelsius(float f);
float celsiusToFahrenheit(float c);

#endif // SLEEPYPOD_API_H
