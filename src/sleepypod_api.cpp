#include "sleepypod_api.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include "config.h"

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
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK)
  {
    String payload = http.getString();
    const char *error = nullptr;
    status = parsePodStatus(payload.c_str(), &error);

    if (error)
    {
      Serial.printf("JSON parse error: %s\n", error);
    }
    else if (status.success)
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
    Serial.printf("GET /api/device/status failed: %d\n", httpCode);
  }

  http.end();
  return status;
}

bool setPodTemperature(IPAddress ip, const char *side, int temperatureF, uint16_t port, int holdMinutes)
{
  if (holdMinutes < 0 || holdMinutes > 1440) return false;
  temperatureF = clampTemperatureF(temperatureF);

  HTTPClient http;
  String url = "http://" + ip.toString() + ":" + String(port) + "/api/device/temperature";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);

  JsonDocument doc;
  doc["side"] = side;
  doc["temperature"] = temperatureF;
  if (holdMinutes > 0) doc["holdMinutes"] = holdMinutes;

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

bool resumePodTemperature(IPAddress ip, const char *side, uint16_t port)
{
  HTTPClient http;
  String url = "http://" + ip.toString() + ":" + String(port) + "/api/device/temperature/resume";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);

  JsonDocument doc;
  doc["side"] = side;
  String payload;
  serializeJson(doc, payload);
  int httpCode = http.POST(payload);
  Serial.printf("Resume %s temperature: %d\n", side, httpCode);
  http.end();
  return httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_NO_CONTENT;
}

bool setPodPower(IPAddress ip, const char *side, bool powered, uint16_t port)
{
  HTTPClient http;
  String url = "http://" + ip.toString() + ":" + String(port) + "/api/device/power";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);

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
  PodSettings settings = parsePodSettings(""); // defaults, success = false

  HTTPClient http;
  String url = "http://" + ip.toString() + ":" + String(port) + "/api/settings";

  http.begin(url);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK)
  {
    String payload = http.getString();
    const char *error = nullptr;
    settings = parsePodSettings(payload.c_str(), &error);

    if (!error)
    {
      Serial.printf("Settings: L='%s' R='%s' unit=%s reboot=%s@%s\n",
                    settings.leftName,
                    settings.rightName,
                    settings.temperatureUnit,
                    settings.rebootDaily ? "yes" : "no",
                    settings.rebootTime);
    }
    else
    {
      Serial.printf("Settings JSON parse error: %s\n", error);
    }
  }
  else
  {
    Serial.printf("GET /api/settings failed: %d\n", httpCode);
  }

  http.end();
  return settings;
}
