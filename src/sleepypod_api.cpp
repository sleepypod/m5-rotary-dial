#include "sleepypod_api.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include "config.h"

// ==================== Temperature Conversion ====================

float fahrenheitToCelsius(float f)
{
  return (f - 32.0f) * 5.0f / 9.0f;
}

float celsiusToFahrenheit(float c)
{
  return (c * 9.0f / 5.0f) + 32.0f;
}

// ==================== mDNS Discovery ====================

bool discoverPod(IPAddress &podIP, uint16_t &podPort)
{
  Serial.println("Searching for sleepypod-core via mDNS...");

  int n = MDNS.queryService("sleepypod", "tcp");

  if (n > 0)
  {
    podIP = MDNS.IP(0);
    podPort = MDNS.port(0);
    Serial.printf("Found Pod at %s:%d\n", podIP.toString().c_str(), podPort);
    return true;
  }

  Serial.println("No Pod found via mDNS");
  return false;
}

// ==================== API Calls ====================

PodStatus fetchPodStatus(IPAddress ip, uint16_t port)
{
  PodStatus status = {};
  status.success = false;

  HTTPClient http;
  String url = "http://" + ip.toString() + ":" + String(port) + "/api/device/status";

  http.begin(url);
  http.setTimeout(5000);

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK)
  {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error)
    {
      // Parse left side - try both possible response formats
      // Format 1 (sleepypod-core REST): { leftSide: { targetTemperature, currentTemperature } }
      // Format 2 (raw/legacy):          { left: { targetTemperatureF, isOn } }
      if (doc["leftSide"].is<JsonObject>())
      {
        JsonObject left = doc["leftSide"];
        status.left.targetTemperatureF = left["targetTemperature"] | left["targetLevel"] | TEMP_DEFAULT_F;
        status.left.currentTemperatureF = left["currentTemperature"] | status.left.targetTemperatureF;
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
        status.left.targetTemperatureF = left["targetTemperatureF"] | left["targetTemperature"] | TEMP_DEFAULT_F;
        status.left.currentTemperatureF = left["currentTemperatureF"] | left["currentTemperature"] | status.left.targetTemperatureF;
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
        status.right.targetTemperatureF = right["targetTemperature"] | right["targetLevel"] | TEMP_DEFAULT_F;
        status.right.currentTemperatureF = right["currentTemperature"] | status.right.targetTemperatureF;
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
        status.right.targetTemperatureF = right["targetTemperatureF"] | right["targetTemperature"] | TEMP_DEFAULT_F;
        status.right.currentTemperatureF = right["currentTemperatureF"] | right["currentTemperature"] | status.right.targetTemperatureF;
        if (right["isOn"].is<bool>())
          status.right.isPowered = right["isOn"].as<bool>();
        else if (right["isPowered"].is<bool>())
          status.right.isPowered = right["isPowered"].as<bool>();
        else
          status.right.isPowered = true;
        status.right.valid = true;
      }

      status.success = (status.left.valid || status.right.valid);

      if (status.success)
      {
        Serial.printf("Pod status: L=%d°F(%s) R=%d°F(%s)\n",
                      status.left.targetTemperatureF,
                      status.left.isPowered ? "ON" : "OFF",
                      status.right.targetTemperatureF,
                      status.right.isPowered ? "ON" : "OFF");
      }
    }
    else
    {
      Serial.printf("JSON parse error: %s\n", error.c_str());
    }
  }
  else
  {
    Serial.printf("GET /api/device/status failed: %d\n", httpCode);
  }

  http.end();
  return status;
}

bool setPodTemperature(IPAddress ip, const char *side, int temperatureF, uint16_t port)
{
  // Clamp to valid range
  if (temperatureF < 55) temperatureF = 55;
  if (temperatureF > 110) temperatureF = 110;

  HTTPClient http;
  String url = "http://" + ip.toString() + ":" + String(port) + "/api/device/temperature";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  JsonDocument doc;
  doc["side"] = side;
  doc["temperature"] = temperatureF;

  String payload;
  serializeJson(doc, payload);

  Serial.printf("POST %s: %s\n", url.c_str(), payload.c_str());

  int httpCode = http.POST(payload);
  bool success = (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_NO_CONTENT);

  if (success)
  {
    Serial.printf("Set %s temperature to %d°F\n", side, temperatureF);
  }
  else
  {
    Serial.printf("Set temperature failed: %d\n", httpCode);
  }

  http.end();
  return success;
}

bool setPodPower(IPAddress ip, const char *side, bool powered, uint16_t port)
{
  HTTPClient http;
  String url = "http://" + ip.toString() + ":" + String(port) + "/api/device/power";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  JsonDocument doc;
  doc["side"] = side;
  doc["powered"] = powered;

  String payload;
  serializeJson(doc, payload);

  Serial.printf("POST %s: %s\n", url.c_str(), payload.c_str());

  int httpCode = http.POST(payload);
  bool success = (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_NO_CONTENT);

  if (success)
  {
    Serial.printf("Set %s power to %s\n", side, powered ? "ON" : "OFF");
  }
  else
  {
    Serial.printf("Set power failed: %d\n", httpCode);
  }

  http.end();
  return success;
}

// ==================== Settings ====================

PodSettings fetchPodSettings(IPAddress ip, uint16_t port)
{
  PodSettings settings = {};
  settings.leftName = "Left";
  settings.rightName = "Right";
  settings.temperatureUnit = "F";
  settings.rebootDaily = false;
  settings.rebootTime = "03:00";
  settings.success = false;

  HTTPClient http;
  String url = "http://" + ip.toString() + ":" + String(port) + "/api/settings";

  http.begin(url);
  http.setTimeout(5000);

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK)
  {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error)
    {
      // Side names
      if (doc["sides"]["left"]["name"].is<const char *>())
        settings.leftName = doc["sides"]["left"]["name"].as<String>();
      if (doc["sides"]["right"]["name"].is<const char *>())
        settings.rightName = doc["sides"]["right"]["name"].as<String>();

      // Device settings
      if (doc["device"]["temperatureUnit"].is<const char *>())
        settings.temperatureUnit = doc["device"]["temperatureUnit"].as<String>();
      if (doc["device"]["rebootDaily"].is<bool>())
        settings.rebootDaily = doc["device"]["rebootDaily"].as<bool>();
      if (doc["device"]["rebootTime"].is<const char *>())
        settings.rebootTime = doc["device"]["rebootTime"].as<String>();

      settings.success = true;

      Serial.printf("Settings: L='%s' R='%s' unit=%s reboot=%s@%s\n",
                    settings.leftName.c_str(),
                    settings.rightName.c_str(),
                    settings.temperatureUnit.c_str(),
                    settings.rebootDaily ? "yes" : "no",
                    settings.rebootTime.c_str());
    }
    else
    {
      Serial.printf("Settings JSON parse error: %s\n", error.c_str());
    }
  }
  else
  {
    Serial.printf("GET /api/settings failed: %d\n", httpCode);
  }

  http.end();
  return settings;
}

// TEMP_DEFAULT_F provided by config.h
