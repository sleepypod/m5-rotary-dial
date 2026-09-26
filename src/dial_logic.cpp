#include "dial_logic.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <ArduinoJson.h>

// Same literal Arduino's PI macro expands to, so the trig below stays
// bit-identical between the firmware and the host build.
static const double kPi = 3.1415926535897932384626433832795;

// ==================== Temperature ====================

int clampTemperatureF(int tempF)
{
  if (tempF < TEMP_MIN_F) tempF = TEMP_MIN_F;
  if (tempF > TEMP_MAX_F) tempF = TEMP_MAX_F;
  return tempF;
}

bool isValidSetpointF(int tempF)
{
  return tempF >= TEMP_MIN_F && tempF <= TEMP_MAX_F;
}

float fahrenheitToCelsius(float f)
{
  return (f - 32.0f) * 5.0f / 9.0f;
}

float celsiusToFahrenheit(float c)
{
  return (c * 9.0f / 5.0f) + 32.0f;
}

// ==================== Arc geometry ====================

float setpointAngle(int tempF)
{
  float pct = (float)(tempF - TEMP_MIN_F) / (float)(TEMP_MAX_F - TEMP_MIN_F);
  if (pct < 0) pct = 0;
  if (pct > 1) pct = 1;
  return ARC_START + pct * ARC_SPAN;
}

int angleSetpoint(float angle)
{
  float pct = (angle - ARC_START) / ARC_SPAN;
  if (pct < 0) pct = 0;
  if (pct > 1) pct = 1;
  return TEMP_MIN_F + (int)lroundf(pct * (float)(TEMP_MAX_F - TEMP_MIN_F));
}

// ==================== Colour ====================

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

uint16_t arcColor(float percent, bool night)
{
  if (percent < 0.0f) percent = 0.0f;
  if (percent > 1.0f) percent = 1.0f;

  if (night)
  {
    uint8_t r = (uint8_t)(0x88 + (0xE0 - 0x88) * percent);
    return rgb565(r, 0, 0);
  }

  static const float stops[5] = {0.0f, 10.0f / 55.0f, 25.0f / 55.0f, 40.0f / 55.0f, 1.0f};
  static const uint8_t rgb[5][3] = {
      {0x3B, 0x8B, 0xFF}, // 55°F
      {0x35, 0xC4, 0xE0}, // 65°F
      {0xE8, 0xE4, 0xDC}, // 80°F
      {0xFF, 0xA5, 0x3C}, // 95°F
      {0xFF, 0x5A, 0x3C}  // 110°F
  };
  int i = 0;
  while (i < 3 && percent > stops[i + 1]) i++;
  float t = (percent - stops[i]) / (stops[i + 1] - stops[i]);
  uint8_t r = rgb[i][0] + (int)((rgb[i + 1][0] - rgb[i][0]) * t);
  uint8_t g = rgb[i][1] + (int)((rgb[i + 1][1] - rgb[i][1]) * t);
  uint8_t b = rgb[i][2] + (int)((rgb[i + 1][2] - rgb[i][2]) * t);
  return rgb565(r, g, b);
}

uint16_t lerp565(uint16_t a, uint16_t b, float t)
{
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = ar + (int)((br - ar) * t);
  int g = ag + (int)((bg - ag) * t);
  int bl = ab + (int)((bb - ab) * t);
  return (r << 11) | (g << 5) | bl;
}

// ==================== Detent acceleration ====================

int DetentAccel::note(unsigned long now)
{
  unsigned long oldest = times[idx];
  times[idx] = now;
  idx = (idx + 1) % 3;
  return (oldest != 0 && now - oldest <= ACCEL_WINDOW_MS) ? 2 : 1;
}

void DetentAccel::reset()
{
  times[0] = times[1] = times[2] = 0;
  idx = 0;
}

// ==================== OFF stop ====================

OffStopAction OffStop::detent(bool powerOn, int currentF, int dir)
{
  if (!powerOn)
  {
    // Any upward detent turns the side back on at its last setpoint
    if (dir > 0)
    {
      accum = 0;
      return OFFSTOP_TURN_ON;
    }
    return OFFSTOP_IGNORE;
  }

  if (dir < 0 && currentF <= TEMP_MIN_F)
  {
    // Past the minimum: count detents toward the OFF stop
    accum++;
    if (accum >= OFF_DETENTS)
    {
      accum = 0;
      return OFFSTOP_TURN_OFF;
    }
    return OFFSTOP_COUNTING;
  }

  accum = 0;
  return OFFSTOP_ADJUST;
}

void OffStop::reset()
{
  accum = 0;
}

// ==================== Loader shimmer ====================

ShimmerParams shimmerAt(unsigned long now, float spanFrom, float spanTo, bool night)
{
  ShimmerParams s;
  const unsigned long travel = night ? 4000 : 2600, rest = 400;
  unsigned long t = now % (travel + rest);
  if (spanTo - spanFrom >= SHIMMER_MIN_SPAN_DEG)
  {
    if (t < travel)
    {
      float p = (float)t / (float)travel;
      p = -(cosf(kPi * p) - 1.0f) / 2.0f; // ease-in-out sine
      float pos = spanFrom + (spanTo - spanFrom) * p;
      s.glow10 = (int)((pos - ARC_START) * 10.0f);
    }
    else
    {
      s.glow10 = SHIMMER_RESTING; // resting: base only, no highlight anywhere
    }
    s.sigma10 = night ? 80 : 60;
  }
  else
  {
    float ph = (float)(now % 2400) / 2400.0f;
    s.pulse = 0.5f + 0.5f * sinf(ph * 2.0f * kPi);
  }
  return s;
}

// ==================== Pod JSON ====================

static void copyStr(char *dst, size_t n, const char *src)
{
  if (!src) src = "";
  strncpy(dst, src, n - 1);
  dst[n - 1] = '\0';
}

// First of the two keys that holds an integer, else fallback. Written out
// rather than `o[a] | o[b] | fallback` because ArduinoJson's chained
// variant|variant treats a literal 0 as "no value", which turned an off
// side's target 0 into TEMP_DEFAULT_F and let it past the setpoint guard.
static int intField(JsonObject o, const char *a, const char *b, int fallback)
{
  if (o[a].is<int>()) return o[a].as<int>();
  if (b && o[b].is<int>()) return o[b].as<int>();
  return fallback;
}

PodStatus parsePodStatus(const char *json, const char **error)
{
  PodStatus status = {};
  status.success = false;
  if (error) *error = nullptr;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json ? json : "");
  if (err)
  {
    if (error) *error = err.c_str();
    return status;
  }

  // Parse left side - try both possible response formats
  // Format 1 (sleepypod-core REST): { leftSide: { targetTemperature, currentTemperature } }
  // Format 2 (raw/legacy):          { left: { targetTemperatureF, isOn } }
  if (doc["leftSide"].is<JsonObject>())
  {
    JsonObject left = doc["leftSide"];
    status.left.targetTemperatureF = intField(left, "targetTemperature", "targetLevel", TEMP_DEFAULT_F);
    status.left.currentTemperatureF = intField(left, "currentTemperature", nullptr, status.left.targetTemperatureF);
    // isPowered: check multiple possible field names
    if (left["isPowered"].is<bool>())
      status.left.isPowered = left["isPowered"].as<bool>();
    else if (left["isOn"].is<bool>())
      status.left.isPowered = left["isOn"].as<bool>();
    else
      status.left.isPowered = (status.left.targetTemperatureF > 0);
    status.left.valid = true;
  }
  else if (doc["left"].is<JsonObject>())
  {
    JsonObject left = doc["left"];
    status.left.targetTemperatureF = intField(left, "targetTemperatureF", "targetTemperature", TEMP_DEFAULT_F);
    status.left.currentTemperatureF = intField(left, "currentTemperatureF", "currentTemperature", status.left.targetTemperatureF);
    if (left["isOn"].is<bool>())
      status.left.isPowered = left["isOn"].as<bool>();
    else if (left["isPowered"].is<bool>())
      status.left.isPowered = left["isPowered"].as<bool>();
    else
      status.left.isPowered = true;
    status.left.valid = true;
  }

  // Parse right side (same dual-format handling)
  if (doc["rightSide"].is<JsonObject>())
  {
    JsonObject right = doc["rightSide"];
    status.right.targetTemperatureF = intField(right, "targetTemperature", "targetLevel", TEMP_DEFAULT_F);
    status.right.currentTemperatureF = intField(right, "currentTemperature", nullptr, status.right.targetTemperatureF);
    if (right["isPowered"].is<bool>())
      status.right.isPowered = right["isPowered"].as<bool>();
    else if (right["isOn"].is<bool>())
      status.right.isPowered = right["isOn"].as<bool>();
    else
      status.right.isPowered = (status.right.targetTemperatureF > 0);
    status.right.valid = true;
  }
  else if (doc["right"].is<JsonObject>())
  {
    JsonObject right = doc["right"];
    status.right.targetTemperatureF = intField(right, "targetTemperatureF", "targetTemperature", TEMP_DEFAULT_F);
    status.right.currentTemperatureF = intField(right, "currentTemperatureF", "currentTemperature", status.right.targetTemperatureF);
    if (right["isOn"].is<bool>())
      status.right.isPowered = right["isOn"].as<bool>();
    else if (right["isPowered"].is<bool>())
      status.right.isPowered = right["isPowered"].as<bool>();
    else
      status.right.isPowered = true;
    status.right.valid = true;
  }

  status.success = (status.left.valid || status.right.valid);
  return status;
}

PodSettings parsePodSettings(const char *json, const char **error)
{
  PodSettings settings = {};
  copyStr(settings.leftName, sizeof(settings.leftName), "Left");
  copyStr(settings.rightName, sizeof(settings.rightName), "Right");
  copyStr(settings.temperatureUnit, sizeof(settings.temperatureUnit), "F");
  settings.rebootDaily = false;
  copyStr(settings.rebootTime, sizeof(settings.rebootTime), "03:00");
  settings.success = false;
  if (error) *error = nullptr;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json ? json : "");
  if (err)
  {
    if (error) *error = err.c_str();
    return settings;
  }

  // Side names
  if (doc["sides"]["left"]["name"].is<const char *>())
    copyStr(settings.leftName, sizeof(settings.leftName), doc["sides"]["left"]["name"].as<const char *>());
  if (doc["sides"]["right"]["name"].is<const char *>())
    copyStr(settings.rightName, sizeof(settings.rightName), doc["sides"]["right"]["name"].as<const char *>());

  // Device settings
  if (doc["device"]["temperatureUnit"].is<const char *>())
    copyStr(settings.temperatureUnit, sizeof(settings.temperatureUnit), doc["device"]["temperatureUnit"].as<const char *>());
  if (doc["device"]["rebootDaily"].is<bool>())
    settings.rebootDaily = doc["device"]["rebootDaily"].as<bool>();
  if (doc["device"]["rebootTime"].is<const char *>())
    copyStr(settings.rebootTime, sizeof(settings.rebootTime), doc["device"]["rebootTime"].as<const char *>());

  settings.success = true;
  return settings;
}

int mergeSetpoint(int localSetpointF, const SideStatus &side)
{
  if (side.valid && isValidSetpointF(side.targetTemperatureF)) return side.targetTemperatureF;
  return localSetpointF;
}

int parseRebootHour(const char *hhmm, int fallback)
{
  if (!hhmm || strlen(hhmm) < 4) return fallback;
  char hh[3] = {hhmm[0], hhmm[1], '\0'};
  return atoi(hh);
}
