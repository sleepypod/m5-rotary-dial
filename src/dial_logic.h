#ifndef DIAL_LOGIC_H
#define DIAL_LOGIC_H

// Hardware-free core of the dial: temperature maths, arc geometry and
// colour, detent acceleration, the OFF stop, the loader shimmer, tweens,
// the night window, backlight timing, and Pod JSON parsing. Plain C++17,
// no Arduino. main.cpp and sleepypod_api.cpp call
// into this; test/test_logic exercises it on the host.

#include <stdint.h>
#include <stddef.h>
#include "config.h"

// ==================== Temperature ====================

int clampTemperatureF(int tempF);
bool isValidSetpointF(int tempF);
float fahrenheitToCelsius(float f);
float celsiusToFahrenheit(float c);

// ==================== Arc geometry ====================

// LovyanGFX angles: 0° = 3 o'clock, clockwise. The arc opens at the bottom.
constexpr float ARC_START = 135.0f; // bottom-left
constexpr float ARC_SPAN = 270.0f;  // opening centred at the bottom

float setpointAngle(int tempF);
int angleSetpoint(float angle); // inverse, rounded to the nearest °F

// ==================== Colour ====================

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b);
uint16_t lerp565(uint16_t a, uint16_t b, float t);
// Arc gradient at percent (0..1 along the arc). Day: five perceptual stops
// from cold-water blue through warm white to red-orange. Night: two-stop red.
uint16_t arcColor(float percent, bool night);

// ==================== Detent acceleration ====================

// Three detents inside ACCEL_WINDOW_MS means the user is spinning: step 2°F.
// Capped at 2 so a spin never overshoots by twenty degrees.
struct DetentAccel
{
  unsigned long times[3] = {0, 0, 0};
  uint8_t idx = 0;
  int note(unsigned long now); // returns the step for this detent (1 or 2)
  void reset();
};

// ==================== OFF stop ====================

enum OffStopAction
{
  OFFSTOP_IGNORE = 0, // side is off, detent went down: nothing
  OFFSTOP_ADJUST,     // normal detent: apply currentF + dir * step
  OFFSTOP_COUNTING,   // below minimum, OFF stop not reached yet (show hint)
  OFFSTOP_TURN_OFF,   // OFF_DETENTS reached below the minimum
  OFFSTOP_TURN_ON     // side was off, any detent up turns it back on
};

struct OffStop
{
  int accum = 0;
  OffStopAction detent(bool powerOn, int currentF, int dir);
  void reset();
};

// ==================== Loader shimmer ====================

// A soft highlight travels along the span (spanFrom..spanTo, absolute
// degrees) in the arc's own direction, eases in and out, rests, restarts.
// Too short a span for a blob pulses instead.
struct ShimmerParams
{
  int glow10 = -1;    // highlight centre relative to ARC_START in 0.1°; <0 none; SHIMMER_RESTING while resting
  int sigma10 = 0;    // highlight width in 0.1°
  float pulse = 0.0f; // 0..1 whole-span pulse when the span is too short
};
constexpr int SHIMMER_RESTING = 0x7FFF;
constexpr float SHIMMER_MIN_SPAN_DEG = 24.0f;
ShimmerParams shimmerAt(unsigned long now, float spanFrom, float spanTo, bool night);

// ==================== Tweens ====================

// Time-based tweens (millis driven, never block).
struct Tween
{
  float from = 0, to = 0;
  unsigned long t0 = 0, dur = 1;
  bool active = false;
};
void tweenStart(Tween &t, float from, float to, unsigned long dur, unsigned long now);
float tweenValue(Tween &t, unsigned long now); // eased; deactivates once past dur
float easeOutCubic(float p);
float easeInOutCubic(float p);

// ==================== Time & backlight ====================

// Night when hour is inside [startHour, endHour), wrapping past midnight
// when startHour > endHour (22..7 is the default).
bool isNightHour(int hour, int startHour, int endHour);
// Dim after DIM_TIMEOUT_MS of inactivity (DIM_TIMEOUT_NIGHT_MS at night),
// never inside settings.
bool shouldDim(bool inSettings, unsigned long sinceActivityMs, bool night);
// While dimmed, input arriving SAFE_WAKE_ARM_MS or more after the dim only
// wakes the screen.
bool safeWakeArmed(unsigned long now, unsigned long dimmedAt);
// "78" in Fahrenheit, "25.6" in Celsius.
void formatTemp(int tempF, bool fahrenheit, char *buf, size_t n);
// IP editor: an octet stepped past either end wraps around.
uint8_t wrapOctet(int value);

// ==================== Pod JSON ====================

enum class TemperatureSource : uint8_t { None, Manual, RunOnce, Autopilot, Schedule, Unknown };
enum class TemperatureBlock : uint8_t { None, Safety, Off, Unknown };

struct TemperatureControl
{
  bool available; // absent on older core versions and before controller startup
  TemperatureSource source;
  TemperatureBlock blocked;
  int64_t holdUntil; // Unix epoch milliseconds; 0 means no usable expiry
};

/** Return the display name of an ownership source, including unknown values. */
const char *temperatureSourceLabel(TemperatureSource source);
/** Cycle the supported hold choices; invalid persisted values restart at 30. */
int nextHoldMinutes(int minutes); // cycle the dial's 15 / 30 / 60 / 120 minute choices

// Commands waiting for debounce. The latest explicit action supersedes a
// conflicting queued action; an already running job finishes before the next.
struct PendingSideWrite
{
  bool temperature = false;
  bool power = false;
  bool resume = false;
  /** Queue a target, retaining a preceding power-on and superseding Resume. */
  void queueTemperature();
  /** Queue explicit power and discard older targets or Resume commands. */
  void queuePower();
  /** Release ownership instead of sending any older queued target or power. */
  void queueResume();
  /** Whether this side has at least one operation waiting for dispatch. */
  bool any() const { return temperature || power || resume; }
};

/** Main-loop confirmation state for one side; never shared across sides. */
struct SideWriteFeedback
{
  bool failed = false;
  bool inFlight = false;
  /** Clear only this side's previous failure when the user supplies new input. */
  void queued();
  /** Mark this side's immutable command snapshot as awaiting confirmation. */
  void started();
  /** Ignore an older job's failure when a newer command is already queued. */
  void completed(bool writeOk, bool statusValid, bool newerPending);
};

/** Format ownership and request progress, retaining a known block as a prefix.
 * localExpiry is HH:MM, or empty when the local clock is unavailable.
 * Output is always terminated when n > 0; n == 0 leaves buf untouched.
 */
void formatControlStatus(const TemperatureControl &control, bool updating, bool failed,
                         const char *localExpiry, char *buf, size_t n);

struct SideStatus
{
  int targetTemperatureF; // 55-110 (0 when the Pod reports the side off)
  int currentTemperatureF;
  bool isPowered;
  bool valid; // true if successfully parsed
  TemperatureControl control;
};

struct PodStatus
{
  SideStatus left;
  SideStatus right;
  bool success; // true if the payload parsed and had at least one side
};

constexpr size_t POD_NAME_MAX = 32; // including the terminator

struct PodSettings
{
  char leftName[POD_NAME_MAX];  // Display name for left side (e.g., "Nick")
  char rightName[POD_NAME_MAX]; // Display name for right side (e.g., "Partner")
  char temperatureUnit[4];      // "F" or "C"
  bool rebootDaily;             // Whether Pod reboots daily
  char rebootTime[8];           // HH:mm format
  bool success;
};

// GET /api/device/status body -> PodStatus. On a JSON error success is
// false and *error (if given) names it.
PodStatus parsePodStatus(const char *json, const char **error = nullptr);

// GET /api/settings body -> PodSettings with defaults for missing fields.
PodSettings parsePodSettings(const char *json, const char **error = nullptr);

// The Pod reports target 0 for a side that is off (and may send other
// out-of-range values); only an in-range target replaces the local setpoint.
int mergeSetpoint(int localSetpointF, const SideStatus &side);

// "HH:mm" -> hour, or fallback when the string is too short to hold one.
int parseRebootHour(const char *hhmm, int fallback);

#endif // DIAL_LOGIC_H
