#ifndef SLEEPYPOD_API_H
#define SLEEPYPOD_API_H

#include <Arduino.h>
#include <IPAddress.h>
#include "dial_logic.h" // SideStatus, PodStatus, PodSettings, parsers, temperature maths

// Discover the Pod on the local network via mDNS (_sleepypod._tcp)
// Returns true if found, sets podIP and podPort
bool discoverPod(IPAddress &podIP, uint16_t &podPort);

// Fetch device status from sleepypod-core
// GET /api/device/status
PodStatus fetchPodStatus(IPAddress ip, uint16_t port = 3000);

// Set temperature for a side
// POST /api/device/temperature
// side: "left" or "right", temperature: 55-110°F
// holdMinutes: 0 omits the optional field for older cores; otherwise 1-1440.
bool setPodTemperature(IPAddress ip, const char *side, int temperatureF, uint16_t port = 3000, int holdMinutes = 0);

// POST /api/device/temperature/resume. Releases ownership without powering on.
bool resumePodTemperature(IPAddress ip, const char *side, uint16_t port = 3000);

// Set power state for a side
// POST /api/device/power
// side: "left" or "right", powered: true/false
bool setPodPower(IPAddress ip, const char *side, bool powered, uint16_t port = 3000);

// Fetch settings from sleepypod-core
// GET /api/settings
PodSettings fetchPodSettings(IPAddress ip, uint16_t port = 3000);

#endif // SLEEPYPOD_API_H
