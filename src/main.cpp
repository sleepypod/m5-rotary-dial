// Sleepypod MT Rotary Dial — M5Stack Dial temperature controller for sleepypod-core
//
// Based on RotaryDial by dallonby (https://github.com/dallonby/RotaryDial)
// Adapted to use sleepypod-core tRPC/REST APIs instead of FreeSleep
//
// Controls left/right sides of an Eight Sleep Pod via sleepypod-core,
// with mDNS auto-discovery, rotary dial interface, and automatic night mode.
//
// Interaction model (main screen):
//   rotate            adjust the active side's setpoint (1°F/detent, 2°F when spun)
//   rotate below min  two extra detents reach the OFF stop; rotating up turns back on
//   click / swipe     cycle side: Left -> Right -> Both -> Left
//   press and hold    1.5s with a progress ring opens settings
// Nothing on the main screen changes a value by touch, and nothing depends on
// a hold duration except settings, which shows its ring.

#include <M5Dial.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include "config.h"
#include "sleepypod_api.h"

Preferences preferences;

// Temperature setpoints (Fahrenheit, 55-110, matching sleepypod-core API)
int leftSetpoint = TEMP_DEFAULT_F;
int rightSetpoint = TEMP_DEFAULT_F;

// Connection state
bool wifiConnected = false;
bool podFound = false;
long lastEncoderPosition = 0;
unsigned long lastActivityTime = 0;
bool isDimmed = false;
unsigned long dimmedAt = 0;
bool timeInitialized = false;
bool inSettingsMenu = false;

// Which side the dial is steering
enum Side : uint8_t
{
  SIDE_LEFT = 0,
  SIDE_RIGHT,
  SIDE_BOTH,
  SIDE_COUNT
};
Side activeSide = SIDE_LEFT;
// In Both mode the numeral shows this value; the first rotation snaps both
// sides to it (plus the step) so they move together from then on
int bothDisplayF = TEMP_DEFAULT_F;

// Night mode override: Auto follows the schedule; On/Off force it
enum NightOverride
{
  NIGHT_AUTO = 0,
  NIGHT_FORCE_ON,
  NIGHT_FORCE_OFF
};
NightOverride nightOverride = NIGHT_AUTO;

// Default side shown at boot (separate from the currently active side)
bool defaultRightSide = false;

// Connection health (shown on the main screen when degraded)
bool podReachable = true;
int podSyncFailures = 0;
unsigned long lastWifiCheck = 0;

// setupMDNS/setupNTP early-return when WiFi is down at boot; this flag
// lets the reconnect paths run them exactly once later
bool networkServicesStarted = false;

// Saved WiFi credentials
String savedWifiSSID = "";
String savedWifiPassword = "";

// Temperature unit setting (true = Fahrenheit, false = Celsius)
bool useFahrenheit = true;  // Default to Fahrenheit (native API unit)
bool unitOverridden = false; // User set the unit locally; don't let Pod sync revert it

// Side names (fetched from sleepypod-core settings)
String leftSideName = "Left";
String rightSideName = "Right";

// Power state per side
bool leftPowerOn = true;
bool rightPowerOn = true;

// Current (actual) temperature per side (from Pod sensors)
int leftCurrentTempF = TEMP_DEFAULT_F;
int rightCurrentTempF = TEMP_DEFAULT_F;

// Auto-restart (daily, for reliability)
bool autoRestartEnabled = false;
int autoRestartHour = 3; // Default 3am
bool restartTriggeredToday = false;
int lastRestartCheckDay = -1;

// Debounced sleepypod-core writes. Local state changes immediately (the arc
// cap is drawn hollow until the Pod confirms); the flush sends whatever is
// pending per side once input has been quiet for API_DEBOUNCE_MS.
unsigned long lastSetpointChangeTime = 0;
bool pendingApiUpdate = false;
bool pendingTemp[2] = {false, false};
bool pendingPower[2] = {false, false};
const unsigned long API_DEBOUNCE_MS = 500;

// Periodic sync from sleepypod-core
unsigned long lastPodSync = 0;
const unsigned long POD_SYNC_INTERVAL_MS = 30000; // 30 seconds

// Track night mode state to detect changes
bool wasNightMode = false;

// Press-and-hold (encoder button or touch) -> settings, with a progress ring
unsigned long holdStartTime = 0;
bool holdActive = false;
bool holdIsTouch = false;
bool holdConsumed = false;

// OFF stop below the minimum setpoint
int offDetentAccum = 0;

// Rotation acceleration: timestamps of the last three detents
unsigned long detentTimes[3] = {0, 0, 0};
uint8_t detentIdx = 0;

// Pod connection
IPAddress podIP(192, 168, 1, 88); // Default Pod IP
uint16_t podPort = POD_API_PORT;

// Menu navigation
enum MenuItem
{
  MENU_WIFI_SETTINGS = 0,
  MENU_POD_IP,
  MENU_MDNS_DISCOVER,
  MENU_TEMP_UNIT,
  MENU_NIGHT_MODE,
  MENU_DEFAULT_SIDE,
  MENU_COUNT
};

enum SubMenu
{
  SUBMENU_NONE = 0,
  SUBMENU_WIFI_SCAN,
  SUBMENU_WIFI_PASSWORD,
  SUBMENU_IP_EDITOR
};

MenuItem currentMenuItem = MENU_WIFI_SETTINGS;
SubMenu currentSubMenu = SUBMENU_NONE;

// IP editor state
int ipEditorOctet = 0;
uint8_t tempIPOctets[4] = {192, 168, 1, 88};

// WiFi scanning
String scannedSSIDs[20];
int scannedSSIDCount = 0;
int selectedSSIDIndex = 0;
String wifiPasswordInput = "";
int passwordCharIndex = 0;
bool pwLongPressFired = false;
const char alphaNumeric[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!@#$%^&*()_+-=[]{}|;:',.<>?/ ";
// Carousel has one extra virtual entry at the end: DEL (backspace)

// Display geometry. LovyanGFX angles: 0° = 3 o'clock, clockwise.
const int centerX = SCREEN_WIDTH / 2;
const int centerY = SCREEN_HEIGHT / 2;
const int ARC_R_OUTER = 108;
const int ARC_R_INNER = 96;
const int ARC_R_MID = (ARC_R_OUTER + ARC_R_INNER) / 2;
const int ARC_CAP_R = (ARC_R_OUTER - ARC_R_INNER) / 2;
const float ARC_START = 135.0f;  // bottom-left
const float ARC_SPAN = 270.0f;   // opening centred at the bottom
const int CURRENT_R = 90;        // ring radius of the "current temperature" marker

// Rendering: one back buffer, redrawn only when something moves
LGFX_Sprite sprite(&M5Dial.Display);
bool uiDirty = true;
unsigned long lastFrameTime = 0;
int lastDrawnMinute = -1;

// Time-based tweens (millis driven, never block)
struct Tween
{
  float from = 0, to = 0;
  unsigned long t0 = 0, dur = 1;
  bool active = false;
};
Tween arcTween;       // arc fill angle
Tween underlineTween; // active-side underline x
Tween underlineWidthTween;
unsigned long lastDetentTime = 0;

// Serial debug channel (see handleSerialDebug): simulated detents feed the
// real encoder path so the OFF stop and acceleration are exercised too
long simulatedEncoderDelta = 0;

// Backlight fade
float brightnessNow = BRIGHTNESS_DAY;
float brightnessTarget = BRIGHTNESS_DAY;
Tween brightnessTween;

// Theme for the current mode
struct Theme
{
  bool night;
  uint16_t bg, track, muted, secondary, text, alert, cool, warm;
};

// ==================== Function Prototypes ====================

void setupWiFi();
void setupMDNS();
void setupNTP();
void drawTemperatureUI();
void renderMainScreen(unsigned long now);
void renderDimScreen();
void drawSettingsMenu();
void drawIPEditor();
void drawWiFiScanner();
void drawPasswordEntry();
void handleEncoderInput();
void handleEncoderInSettings();
void handleEncoderInIPEditor();
void handleEncoderInWiFiScanner();
void handleEncoderInPasswordEntry();
void handleTouchInput();
void handleHold(unsigned long now);
void updateBrightness();
void recordActivity();
bool isNightTime();
Theme currentTheme();
uint16_t arcColor(float percent, const Theme &th);
float mapFloat(float x, float in_min, float in_max, float out_min, float out_max);
int &getActiveSetpoint();
int getDisplaySetpoint();
bool isActivePowerOn();
String getMenuItemName(MenuItem item);
void feedbackBeep(uint16_t freq);
void drawBusyScreen(const char *msg);
String fitText(const String &s, unsigned int maxChars);
void saveIPFromEditor();
void startIPEditor();
void startWiFiScanner();
void startPasswordEntry();
void syncFromPod();
void syncStatusFromPod();
void applySetpoint(int newTemp);
void setActivePower(bool on);
void cycleSide(int direction);
void openSettings();
bool consumeSafeWake();
void noteDetent(unsigned long now, int &stepSize);
void tweenStart(Tween &t, float from, float to, unsigned long dur, unsigned long now);
float tweenValue(Tween &t, unsigned long now);
float setpointAngle(int tempF);
void flushPendingApi();
bool notePodRequestResult(bool ok);
void handleSerialDebug();
void dumpScreen();

// ==================== Setup ====================

void setup()
{
  auto cfg = M5.config();
  M5Dial.begin(cfg, true, false); // Enable encoder, disable RFID

  Serial.begin(115200);
  Serial.println("\n\nSleepypod MT Rotary Dial");
  Serial.println("=======================");
  Serial.println("Based on RotaryDial by dallonby");
  Serial.println("https://github.com/dallonby/RotaryDial");

  // Load saved settings from NVS
  preferences.begin("sleepypod", false);

  // Clear stale NVS WiFi from previous firmware if migrating
  uint8_t fwVersion = preferences.getUChar("fwVer", 0);
  if (fwVersion < 1)
  {
    // First boot of this firmware — clear old WiFi creds so credentials.h is used
    preferences.remove("wifiSSID");
    preferences.remove("wifiPass");
    preferences.putUChar("fwVer", 1);
    Serial.println("First boot: cleared stale NVS WiFi credentials");
  }

  podIP = IPAddress(
      preferences.getUChar("podIP0", 192),
      preferences.getUChar("podIP1", 168),
      preferences.getUChar("podIP2", 1),
      preferences.getUChar("podIP3", 88));
  podPort = preferences.getUShort("podPort", POD_API_PORT);
  Serial.printf("Loaded Pod IP: %s:%d\n", podIP.toString().c_str(), podPort);

  // Load saved WiFi credentials
  savedWifiSSID = preferences.getString("wifiSSID", "");
  savedWifiPassword = preferences.getString("wifiPass", "");

  // Load temperature unit setting
  useFahrenheit = preferences.getBool("useFahrenheit", true);
  unitOverridden = preferences.getBool("unitOverride", false);

  // Load night mode override (validate — NVS could hold anything)
  uint8_t savedNightOvr = preferences.getUChar("nightOvr", 0);
  if (savedNightOvr > NIGHT_FORCE_OFF) savedNightOvr = NIGHT_AUTO;
  nightOverride = (NightOverride)savedNightOvr;

  // Load default side ("Both" is never the boot default — a bump must not
  // reach a sleeping partner's side)
  defaultRightSide = preferences.getBool("rightSide", false);
  activeSide = defaultRightSide ? SIDE_RIGHT : SIDE_LEFT;
  Serial.printf("Default side: %s\n", defaultRightSide ? "Right" : "Left");

  // Load cached side names (updated from Pod settings on sync)
  leftSideName = preferences.getString("leftName", "Left");
  rightSideName = preferences.getString("rightName", "Right");
  Serial.printf("Side names: L='%s' R='%s'\n", leftSideName.c_str(), rightSideName.c_str());

  // Initialize display
  M5Dial.Display.setRotation(0);
  M5Dial.Display.fillScreen(UI_BG);
  M5Dial.Display.setTextColor(UI_SECONDARY);
  M5Dial.Display.setTextDatum(middle_center);

  // Create sprite for double buffering (internal RAM so pushSprite can DMA)
  sprite.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT);

  // Piezo: quiet, short ticks only
  M5Dial.Speaker.setVolume(40);

  // Touch: a flick must travel a bit before it counts as a swipe
  M5Dial.Touch.setFlickThresh(16);

  // Show startup message
  M5Dial.Display.setFont(&fonts::FreeSans9pt7b);
  M5Dial.Display.drawString("Connecting", centerX, centerY);

  // Connect to WiFi
  setupWiFi();

  // Setup mDNS for Pod discovery and self-announcement
  setupMDNS();

  // Setup NTP time sync
  setupNTP();

  // Sync status from Pod
  if (wifiConnected)
  {
    syncStatusFromPod();
    networkServicesStarted = true;
  }
  bothDisplayF = getActiveSetpoint();

  // Get initial encoder position
  lastEncoderPosition = M5Dial.Encoder.read();
  lastActivityTime = millis();
  wasNightMode = isNightTime();
  brightnessNow = wasNightMode ? BRIGHTNESS_NIGHT : BRIGHTNESS_DAY;
  brightnessTarget = brightnessNow;
  brightnessTween.to = sqrtf(brightnessNow);
  M5Dial.Display.setBrightness((uint8_t)brightnessNow);

  arcTween.to = setpointAngle(getDisplaySetpoint());
  drawTemperatureUI();
}

// ==================== Main Loop ====================

void loop()
{
  M5Dial.update();
  unsigned long currentMillis = millis();

  handleSerialDebug();

  // Encoder is polled (GPIO 40/41 have no interrupt slots), so read every pass
  handleEncoderInput();
  handleTouchInput();
  handleHold(currentMillis);

  updateBrightness();

  // WiFi health check + auto-reconnect
  if (currentMillis - lastWifiCheck >= 10000)
  {
    lastWifiCheck = currentMillis;
    bool nowConnected = (WiFi.status() == WL_CONNECTED);
    if (nowConnected != wifiConnected)
    {
      wifiConnected = nowConnected;
      Serial.printf("WiFi %s\n", nowConnected ? "reconnected" : "lost");
      if (nowConnected && !networkServicesStarted)
      {
        // Boot was offline — mDNS and NTP never started
        setupMDNS();
        setupNTP();
        syncStatusFromPod();
        networkServicesStarted = true;
      }
      drawTemperatureUI();
    }
    if (!nowConnected)
    {
      WiFi.reconnect();
    }
  }

  // Handle debounced API updates
  if (pendingApiUpdate && (currentMillis - lastSetpointChangeTime >= API_DEBOUNCE_MS))
  {
    flushPendingApi();
  }

  // Periodic sync from Pod (back off after repeated failures so a dead
  // Pod doesn't freeze the UI every 30 seconds). Remote state never
  // overwrites a setpoint the user touched in the last 30s.
  unsigned long syncInterval = POD_SYNC_INTERVAL_MS * (podSyncFailures >= 3 ? 4 : 1);
  if (wifiConnected && !inSettingsMenu && !pendingApiUpdate &&
      (currentMillis - lastActivityTime >= REMOTE_SYNC_HOLDOFF_MS) &&
      (currentMillis - lastPodSync >= syncInterval))
  {
    lastPodSync = currentMillis;
    syncFromPod();
  }

  // Check for night mode changes
  if (!inSettingsMenu)
  {
    bool currentNightMode = isNightTime();
    if (currentNightMode != wasNightMode)
    {
      wasNightMode = currentNightMode;
      drawTemperatureUI();
    }
  }

  // Auto-restart check (daily, synced from Pod's reboot schedule)
  if (autoRestartEnabled && timeInitialized)
  {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo))
    {
      // Reset trigger flag when the day changes
      if (timeinfo.tm_yday != lastRestartCheckDay)
      {
        restartTriggeredToday = false;
        lastRestartCheckDay = timeinfo.tm_yday;
      }

      // Restart at the configured hour (first minute of the hour)
      if (!restartTriggeredToday &&
          timeinfo.tm_hour == autoRestartHour &&
          timeinfo.tm_min == 0)
      {
        restartTriggeredToday = true;
        Serial.println("Auto-restart triggered");
        delay(500);
        ESP.restart();
      }
    }
  }

  // ---- Frame scheduling (main screen only) ----
  if (!inSettingsMenu)
  {
    if (isDimmed)
    {
      if (uiDirty)
      {
        uiDirty = false;
        renderDimScreen();
      }
    }
    else
    {
      bool animating = arcTween.active || underlineTween.active || holdActive;
      bool breathing = isActivePowerOn() && podReachable && wifiConnected;
      if (breathing)
      {
        int diff = getDisplaySetpoint() - (activeSide == SIDE_RIGHT ? rightCurrentTempF : leftCurrentTempF);
        breathing = abs(diff) > 1;
      }
      bool minuteChanged = false;
      if (timeInitialized && currentMillis - lastFrameTime >= 1000)
      {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo) && timeinfo.tm_min != lastDrawnMinute) minuteChanged = true;
      }
      unsigned long frameInterval = animating ? 16 : (breathing ? 50 : 1000);
      if ((uiDirty || animating || breathing || minuteChanged) &&
          currentMillis - lastFrameTime >= frameInterval)
      {
        uiDirty = false;
        lastFrameTime = currentMillis;
        renderMainScreen(currentMillis);
      }
    }
  }

  delay(1);
}


// ==================== WiFi & Network ====================

void setupWiFi()
{
  const char *ssid = savedWifiSSID.length() > 0 ? savedWifiSSID.c_str() : WIFI_SSID;
  const char *password = savedWifiPassword.length() > 0 ? savedWifiPassword.c_str() : WIFI_PASSWORD;

  Serial.printf("Connecting to WiFi: %s\n", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30)
  {
    delay(500);
    Serial.print(".");
    attempts++;

    sprite.fillSprite(COLOR_BACKGROUND);
    sprite.setTextColor(COLOR_TEXT);
    sprite.setTextDatum(middle_center);
    sprite.setFont(&fonts::Font0);
    sprite.drawString("Connecting to WiFi", centerX, centerY - 25);
    sprite.drawString(fitText(String(ssid), 24).c_str(), centerX, centerY - 5);
    String dots = "";
    for (int i = 0; i < (attempts % 4); i++)
      dots += ".";
    sprite.drawString(dots.c_str(), centerX, centerY + 20);
    sprite.pushSprite(0, 0);
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    wifiConnected = true;
    Serial.printf("\nWiFi Connected! IP: %s\n", WiFi.localIP().toString().c_str());

    sprite.fillSprite(COLOR_BACKGROUND);
    sprite.setTextColor(COLOR_SETPOINT);
    sprite.setTextDatum(middle_center);
    sprite.setFont(&fonts::Font0);
    sprite.drawString("WiFi Connected!", centerX, centerY - 20);
    sprite.setTextColor(COLOR_TEXT);
    sprite.drawString(WiFi.localIP().toString().c_str(), centerX, centerY + 10);
    sprite.pushSprite(0, 0);
    delay(1500);
  }
  else
  {
    wifiConnected = false;
    Serial.println("\nWiFi Failed!");

    sprite.fillSprite(COLOR_BACKGROUND);
    sprite.setTextColor(COLOR_ARC_HOT);
    sprite.setTextDatum(middle_center);
    sprite.setFont(&fonts::Font0);
    sprite.drawString("WiFi Failed!", centerX, centerY - 25);
    sprite.setTextColor(COLOR_TEXT);
    sprite.drawString("Running offline", centerX, centerY - 5);
    sprite.drawString("Hold center for settings", centerX, centerY + 20);
    sprite.pushSprite(0, 0);
    delay(2500);
  }
}

void setupMDNS()
{
  if (!wifiConnected) return;

  if (MDNS.begin("sleepypod-dial"))
  {
    Serial.println("mDNS responder started: sleepypod-dial.local");

    // Try to discover the Pod
    IPAddress discoveredIP;
    uint16_t discoveredPort;
    if (discoverPod(discoveredIP, discoveredPort))
    {
      podIP = discoveredIP;
      podPort = discoveredPort;
      podFound = true;

      // Save discovered IP
      preferences.putUChar("podIP0", podIP[0]);
      preferences.putUChar("podIP1", podIP[1]);
      preferences.putUChar("podIP2", podIP[2]);
      preferences.putUChar("podIP3", podIP[3]);
      preferences.putUShort("podPort", podPort);

      Serial.printf("Pod discovered and saved: %s:%d\n", podIP.toString().c_str(), podPort);
    }
    else
    {
      Serial.printf("Using saved Pod IP: %s:%d\n", podIP.toString().c_str(), podPort);
    }
  }
}

// ==================== Input: main screen ====================

// While dimmed, input arriving more than SAFE_WAKE_ARM_MS after the dim
// only wakes the screen (a bump in the dark must never change anything).
// Input shortly after dimming acts normally: the user is clearly still there.
// Returns true when the input was consumed by the wake.
bool consumeSafeWake()
{
  if (!isDimmed) return false;
  bool armed = (millis() - dimmedAt) >= SAFE_WAKE_ARM_MS;
  recordActivity();
  return armed;
}

// Records a detent and decides its step size: three detents inside
// ACCEL_WINDOW_MS means the user is spinning, so step 2°F. Capped at 2 so
// a spin never overshoots by twenty degrees.
void noteDetent(unsigned long now, int &stepSize)
{
  unsigned long oldest = detentTimes[detentIdx];
  detentTimes[detentIdx] = now;
  detentIdx = (detentIdx + 1) % 3;
  stepSize = (oldest != 0 && now - oldest <= ACCEL_WINDOW_MS) ? 2 : 1;
  lastDetentTime = now;
}

void applySetpoint(int newTemp)
{
  if (newTemp < TEMP_MIN_F) newTemp = TEMP_MIN_F;
  if (newTemp > TEMP_MAX_F) newTemp = TEMP_MAX_F;

  if (activeSide == SIDE_BOTH)
  {
    leftSetpoint = rightSetpoint = bothDisplayF = newTemp;
    pendingTemp[0] = pendingTemp[1] = true;
  }
  else
  {
    getActiveSetpoint() = newTemp;
    pendingTemp[activeSide == SIDE_RIGHT ? 1 : 0] = true;
  }

  Serial.printf("Setpoint %s -> %d°F\n",
                activeSide == SIDE_BOTH ? "both" : (activeSide == SIDE_RIGHT ? "right" : "left"), newTemp);

  lastSetpointChangeTime = millis();
  pendingApiUpdate = true;
  drawTemperatureUI();
}

// Local power state flips immediately; the arc cap stays hollow until the
// Pod confirms on the next flush.
void setActivePower(bool on)
{
  if (activeSide == SIDE_LEFT || activeSide == SIDE_BOTH)
  {
    leftPowerOn = on;
    pendingPower[0] = true;
  }
  if (activeSide == SIDE_RIGHT || activeSide == SIDE_BOTH)
  {
    rightPowerOn = on;
    pendingPower[1] = true;
  }
  Serial.printf("Power %s\n", on ? "ON" : "OFF");
  feedbackBeep(on ? 2600 : 1800);
  lastSetpointChangeTime = millis();
  pendingApiUpdate = true;
  drawTemperatureUI();
}

void cycleSide(int direction)
{
  unsigned long now = millis();
  float fromAngle = tweenValue(arcTween, now);

  activeSide = (Side)(((int)activeSide + direction + SIDE_COUNT) % SIDE_COUNT);
  if (activeSide == SIDE_BOTH)
  {
    // Carry the side we just left; the first rotation pulls both to it
    bothDisplayF = (direction > 0) ? rightSetpoint : leftSetpoint;
  }
  offDetentAccum = 0;
  Serial.printf("Side -> %s\n",
                activeSide == SIDE_BOTH ? "both" : (activeSide == SIDE_RIGHT ? "right" : "left"));

  tweenStart(arcTween, fromAngle, setpointAngle(getDisplaySetpoint()), SIDE_SWITCH_MS, now);
  feedbackBeep(2200);
  drawTemperatureUI();
}

void openSettings()
{
  inSettingsMenu = true;
  currentMenuItem = MENU_WIFI_SETTINGS;
  currentSubMenu = SUBMENU_NONE;
  lastEncoderPosition = M5Dial.Encoder.read();
  feedbackBeep(2200);
  drawSettingsMenu();
}

void handleEncoderInput()
{
  if (inSettingsMenu)
  {
    if (currentSubMenu == SUBMENU_IP_EDITOR)
      handleEncoderInIPEditor();
    else if (currentSubMenu == SUBMENU_WIFI_SCAN)
      handleEncoderInWiFiScanner();
    else if (currentSubMenu == SUBMENU_WIFI_PASSWORD)
      handleEncoderInPasswordEntry();
    else
      handleEncoderInSettings();
    return;
  }

  long newPosition = M5Dial.Encoder.read();
  long diff = newPosition - lastEncoderPosition;
  if (simulatedEncoderDelta != 0)
  {
    diff += simulatedEncoderDelta;
    simulatedEncoderDelta = 0;
  }

  if (diff != 0)
  {
    lastEncoderPosition = newPosition;
    unsigned long now = millis();

    if (consumeSafeWake()) return;
    recordActivity();
    if (holdActive) return; // press-and-rotate is reserved; ignore for now

    int dir = diff > 0 ? 1 : -1;
    int count = abs((int)diff);
    for (int i = 0; i < count; i++)
    {
      int step = 1;
      noteDetent(now, step);

      bool powerOn = isActivePowerOn();
      int current = getDisplaySetpoint();

      if (!powerOn)
      {
        // OFF stop: any upward detent turns the side back on at its last setpoint
        if (dir > 0)
        {
          offDetentAccum = 0;
          setActivePower(true);
        }
        continue;
      }

      if (dir < 0 && current <= TEMP_MIN_F)
      {
        // Past the minimum: count detents toward the OFF stop
        offDetentAccum++;
        if (offDetentAccum >= OFF_DETENTS)
        {
          offDetentAccum = 0;
          setActivePower(false);
        }
        else
        {
          drawTemperatureUI(); // show the "off" hint building
        }
        continue;
      }

      offDetentAccum = 0;
      applySetpoint(current + dir * step);
    }
  }
}

// Press-and-hold on the encoder button or anywhere on the screen. A short
// press is a click (cycle side); holding shows a ring and opens settings
// when it completes. Releasing early does nothing.
void handleHold(unsigned long now)
{
  if (inSettingsMenu) return;

  bool btn = M5Dial.BtnA.isPressed();
  auto touch = M5Dial.Touch.getDetail();
  bool tch = touch.isPressed();

  if (!holdActive)
  {
    if (btn || tch)
    {
      if (consumeSafeWake())
      {
        holdConsumed = true; // wake only; ignore this press entirely
      }
      else
      {
        holdConsumed = false;
        recordActivity();
      }
      holdActive = true;
      holdIsTouch = tch && !btn;
      holdStartTime = now;
    }
    return;
  }

  bool stillHeld = holdIsTouch ? tch : btn;
  unsigned long held = now - holdStartTime;

  if (stillHeld)
  {
    if (!holdConsumed && held >= SETTINGS_HOLD_MS)
    {
      holdActive = false;
      holdConsumed = true;
      openSettings();
    }
    else if (holdIsTouch && (touch.isFlicking() || touch.wasFlicked()))
    {
      holdConsumed = true; // a swipe is not a hold
    }
    return;
  }

  // Released
  holdActive = false;
  if (holdConsumed) return;
  if (!holdIsTouch && held < CLICK_MAX_MS)
  {
    cycleSide(+1);
  }
  else
  {
    drawTemperatureUI(); // clear the partial ring
  }
}

// ==================== Touch Input ====================

void handleTouchInput()
{
  auto touch = M5Dial.Touch.getDetail();

  if (inSettingsMenu)
  {
    if (!touch.wasPressed()) return;
    recordActivity();

    if (currentSubMenu == SUBMENU_IP_EDITOR)
    {
      // Tap saves, as the on-screen hint promises
      saveIPFromEditor();
      currentSubMenu = SUBMENU_NONE;
      lastEncoderPosition = M5Dial.Encoder.read();
      drawSettingsMenu();
    }
    else if (currentSubMenu != SUBMENU_NONE)
    {
      currentSubMenu = SUBMENU_NONE;
      lastEncoderPosition = M5Dial.Encoder.read();
      drawSettingsMenu();
    }
    else
    {
      inSettingsMenu = false;
      lastEncoderPosition = M5Dial.Encoder.read();
      holdActive = false;
      holdConsumed = true; // the exit tap must not register as a hold
      drawTemperatureUI();
    }
    return;
  }

  // Main screen: the only touch verb is a horizontal swipe to change side.
  // Presses and holds are handled by handleHold().
  if (touch.wasFlicked())
  {
    if (isDimmed) return; // handleHold already woke the screen
    int dx = touch.distanceX();
    int dy = touch.distanceY();
    if (abs(dx) >= SWIPE_MIN_PX && abs(dx) > abs(dy))
    {
      holdConsumed = true;
      cycleSide(dx > 0 ? +1 : -1);
    }
  }
}


// ==================== Display: main screen ====================

// Callers that changed state ask for a frame; the loop renders it on the
// next pass (or animates toward it).
void drawTemperatureUI()
{
  uiDirty = true;
}

void tweenStart(Tween &t, float from, float to, unsigned long dur, unsigned long now)
{
  t.from = from;
  t.to = to;
  t.t0 = now;
  t.dur = dur < 1 ? 1 : dur;
  t.active = (from != to);
}

static float easeOutCubic(float p) { return 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p); }
static float easeInOutCubic(float p)
{
  return p < 0.5f ? 4.0f * p * p * p : 1.0f - powf(-2.0f * p + 2.0f, 3.0f) / 2.0f;
}

float tweenValue(Tween &t, unsigned long now)
{
  if (!t.active) return t.to;
  float p = (float)(now - t.t0) / (float)t.dur;
  if (p >= 1.0f)
  {
    t.active = false;
    return t.to;
  }
  return t.from + (t.to - t.from) * easeInOutCubic(p);
}

float setpointAngle(int tempF)
{
  float pct = (float)(tempF - TEMP_MIN_F) / (float)(TEMP_MAX_F - TEMP_MIN_F);
  if (pct < 0) pct = 0;
  if (pct > 1) pct = 1;
  return ARC_START + pct * ARC_SPAN;
}

Theme currentTheme()
{
  Theme th;
  th.night = isNightTime();
  if (th.night)
  {
    th.bg = UI_N_BG;
    th.track = UI_N_LOW;
    th.muted = UI_N_LOW;
    th.secondary = UI_N_MID;
    th.text = UI_N_HIGH;
    th.alert = UI_N_HIGH;
    th.cool = UI_N_MID;
    th.warm = UI_N_MID;
  }
  else
  {
    th.bg = UI_BG;
    th.track = UI_TRACK;
    th.muted = UI_MUTED;
    th.secondary = UI_SECONDARY;
    th.text = UI_TEXT;
    th.alert = UI_ALERT;
    th.cool = UI_COOL;
    th.warm = UI_WARM;
  }
  return th;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Arc gradient: five perceptual stops, cold-water blue through a body-neutral
// warm white to red-orange. Night is a two-stop red ramp.
uint16_t arcColor(float percent, const Theme &th)
{
  if (percent < 0.0f) percent = 0.0f;
  if (percent > 1.0f) percent = 1.0f;

  if (th.night)
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

static uint16_t lerp565(uint16_t a, uint16_t b, float t)
{
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = ar + (int)((br - ar) * t);
  int g = ag + (int)((bg - ag) * t);
  int bl = ab + (int)((bb - ab) * t);
  return (r << 11) | (g << 5) | bl;
}

static void polar(float deg, int r, int &x, int &y)
{
  float rad = deg * PI / 180.0f;
  x = centerX + (int)lroundf(cosf(rad) * r);
  y = centerY + (int)lroundf(sinf(rad) * r);
}

// Thin anti-aliased ring segment built from short wide-line pieces
static void drawThinArc(float a0, float a1, int r, float halfWidth, uint16_t color)
{
  if (a1 < a0) { float t = a0; a0 = a1; a1 = t; }
  float step = 4.0f;
  int px, py;
  polar(a0, r, px, py);
  for (float a = a0 + step; a < a1 + step; a += step)
  {
    float aa = a > a1 ? a1 : a;
    int x, y;
    polar(aa, r, x, y);
    sprite.drawWideLine(px, py, x, y, halfWidth, color);
    px = x;
    py = y;
    if (aa >= a1) break;
  }
}

// Draws the numeral with a small degree mark and unit to its upper right
static void drawTemperatureNumeral(int tempF, int cx, int cy, const lgfx::IFont *font,
                                   uint16_t color, uint16_t unitColor, bool showUnit)
{
  char buf[10];
  if (useFahrenheit) snprintf(buf, sizeof(buf), "%d", tempF);
  else snprintf(buf, sizeof(buf), "%.1f", fahrenheitToCelsius((float)tempF));

  sprite.setFont(font);
  sprite.setTextColor(color);
  sprite.setTextDatum(middle_center);
  sprite.drawString(buf, cx, cy);

  if (!showUnit) return;
  int w = sprite.textWidth(buf);
  int h = sprite.fontHeight();
  int ux = cx + w / 2 + 6;
  int uy = cy - h / 2 + 12;
  sprite.drawCircle(ux + 3, uy - 6, 3, unitColor);
  sprite.setFont(&fonts::FreeSans12pt7b);
  sprite.setTextColor(unitColor);
  sprite.setTextDatum(middle_left);
  sprite.drawString(useFahrenheit ? "F" : "C", ux + 8, uy);
  sprite.setTextDatum(middle_center);
}

// "label · value" on one line, centred. Fonts lack a middle dot, so draw one.
static void drawStatusLine(const char *label, const char *value, int y, uint16_t color)
{
  sprite.setFont(&fonts::FreeSans9pt7b);
  sprite.setTextColor(color);
  sprite.setTextDatum(middle_center);
  if (!value || !*value)
  {
    sprite.drawString(label, centerX, y);
    return;
  }
  int wl = sprite.textWidth(label);
  int wv = sprite.textWidth(value);
  const int gap = 14;
  int total = wl + gap + wv;
  int x0 = centerX - total / 2;
  sprite.setTextDatum(middle_left);
  sprite.drawString(label, x0, y);
  sprite.fillSmoothCircle(x0 + wl + gap / 2, y, 1, color);
  sprite.drawString(value, x0 + wl + gap, y);
  sprite.setTextDatum(middle_center);
}

static void formatTemp(int tempF, char *buf, size_t n)
{
  if (useFahrenheit) snprintf(buf, n, "%d", tempF);
  else snprintf(buf, n, "%.1f", fahrenheitToCelsius((float)tempF));
}

void renderMainScreen(unsigned long now)
{
  Theme th = currentTheme();
  sprite.fillSprite(th.bg);

  int shownF = getDisplaySetpoint();
  bool powerOn = isActivePowerOn();
  bool online = wifiConnected && podReachable;
  bool unconfirmed = pendingApiUpdate || !online;

  // Arc target: snap while the encoder is moving, settle once it stops
  float targetAngle = powerOn ? setpointAngle(shownF) : ARC_START;
  if (!arcTween.active && arcTween.to != targetAngle)
  {
    bool moving = (now - lastDetentTime) < 100;
    if (moving) arcTween.to = targetAngle;
    else tweenStart(arcTween, arcTween.to, targetAngle, ARC_SETTLE_MS, now);
  }
  else if (arcTween.active && arcTween.to != targetAngle)
  {
    tweenStart(arcTween, tweenValue(arcTween, now), targetAngle, ARC_SETTLE_MS, now);
  }
  float fillAngle = tweenValue(arcTween, now);

  // ---- Track ----
  sprite.fillArc(centerX, centerY, ARC_R_OUTER, ARC_R_INNER, ARC_START, ARC_START + ARC_SPAN, th.track);
  {
    int x, y;
    polar(ARC_START, ARC_R_MID, x, y);
    sprite.fillSmoothCircle(x, y, ARC_CAP_R, th.track);
    polar(ARC_START + ARC_SPAN, ARC_R_MID, x, y);
    sprite.fillSmoothCircle(x, y, ARC_CAP_R, th.track);
  }

  // ---- Setpoint fill (gradient in 3° pieces) ----
  uint16_t capColor = th.track;
  if (powerOn && fillAngle > ARC_START + 0.5f)
  {
    uint16_t fillDim = wifiConnected ? 0 : th.muted;
    for (float a = ARC_START; a < fillAngle; a += 3.0f)
    {
      float segEnd = a + 3.5f;
      if (segEnd > fillAngle) segEnd = fillAngle + 0.5f;
      float pct = (a - ARC_START) / ARC_SPAN;
      uint16_t c = wifiConnected ? arcColor(pct, th) : fillDim;
      sprite.fillArc(centerX, centerY, ARC_R_OUTER, ARC_R_INNER, a, segEnd, c);
    }
    int x, y;
    polar(ARC_START, ARC_R_MID, x, y);
    sprite.fillSmoothCircle(x, y, ARC_CAP_R, wifiConnected ? arcColor(0.0f, th) : fillDim);

    capColor = wifiConnected ? arcColor((fillAngle - ARC_START) / ARC_SPAN, th) : fillDim;
    polar(fillAngle, ARC_R_MID, x, y);
    sprite.fillSmoothCircle(x, y, ARC_CAP_R, capColor);
    if (unconfirmed)
    {
      // Hollow cap: sent but not yet acknowledged, or Pod unreachable
      sprite.fillSmoothCircle(x, y, ARC_CAP_R - 2, th.bg);
    }
  }

  // ---- Current mattress temperature marker + distance-to-go ----
  int currentF = (activeSide == SIDE_RIGHT) ? rightCurrentTempF : leftCurrentTempF;
  int diff = shownF - currentF;
  bool converging = powerOn && online && abs(diff) > 1 && activeSide != SIDE_BOTH;
  if (online && activeSide != SIDE_BOTH && (converging || !powerOn))
  {
    float curAngle = setpointAngle(currentF);
    uint16_t statusColor = diff > 0 ? th.warm : th.cool;
    uint16_t markerColor = th.secondary;
    if (converging)
    {
      unsigned long period = th.night ? BREATH_PERIOD_NIGHT_MS : BREATH_PERIOD_DAY_MS;
      float phase = (float)(now % period) / (float)period;
      float breath = 0.5f + 0.5f * sinf(phase * 2.0f * PI);
      markerColor = lerp565(th.muted, statusColor, breath);
      drawThinArc(curAngle, fillAngle, CURRENT_R, 1.0f, markerColor);
    }
    int x, y;
    polar(curAngle, CURRENT_R, x, y);
    sprite.fillSmoothCircle(x, y, 3, powerOn ? markerColor : th.secondary);
  }

  // ---- Side pair at the top ----
  {
    String ln = fitText(leftSideName, 8);
    String rn = fitText(rightSideName, 8);
    sprite.setFont(&fonts::FreeSans9pt7b);
    int wl = sprite.textWidth(ln.c_str());
    int wr = sprite.textWidth(rn.c_str());
    const int gap = 22;
    int x0 = centerX - (wl + gap + wr) / 2;
    int lcx = x0 + wl / 2;
    int rcx = x0 + wl + gap + wr / 2;
    const int y = 50;

    bool lActive = activeSide != SIDE_RIGHT;
    bool rActive = activeSide != SIDE_LEFT;
    sprite.setTextDatum(middle_center);
    sprite.setTextColor(lActive ? th.text : (leftPowerOn ? th.secondary : th.muted));
    sprite.drawString(ln.c_str(), lcx, y);
    sprite.setTextColor(rActive ? th.text : (rightPowerOn ? th.secondary : th.muted));
    sprite.drawString(rn.c_str(), rcx, y);
    sprite.fillSmoothCircle(x0 + wl + gap / 2, y, 1, th.muted);

    // Underline slides to the active name; spans both in Both mode
    float targetX = activeSide == SIDE_LEFT ? lcx : (activeSide == SIDE_RIGHT ? rcx : (lcx + rcx) / 2.0f);
    float targetW = activeSide == SIDE_BOTH ? (rcx - lcx + 22) : 22.0f;
    if (underlineTween.to == 0) { underlineTween.to = targetX; underlineWidthTween.to = targetW; }
    if (!underlineTween.active && underlineTween.to != targetX)
    {
      tweenStart(underlineTween, underlineTween.to, targetX, SIDE_SWITCH_MS, now);
      tweenStart(underlineWidthTween, underlineWidthTween.to, targetW, SIDE_SWITCH_MS, now);
    }
    float ux = tweenValue(underlineTween, now);
    float uw = tweenValue(underlineWidthTween, now);
    sprite.fillSmoothRoundRect((int)(ux - uw / 2), 60, (int)uw, 2, 1, th.text);
  }

  // ---- Numeral ----
  uint16_t numColor = powerOn ? (online ? th.text : th.secondary) : th.muted;
  drawTemperatureNumeral(shownF, centerX, 116, &fonts::DejaVu56, numColor, th.secondary, true);

  // ---- Status line ----
  {
    const int y = 160;
    char val[12];
    if (!wifiConnected)
    {
      // handled below, at the clock position
    }
    else if (!podReachable)
    {
      drawStatusLine("Pod offline", "", y, th.alert);
    }
    else if (activeSide == SIDE_BOTH && leftSetpoint != rightSetpoint)
    {
      char l[8], r[8];
      formatTemp(leftSetpoint, l, sizeof(l));
      formatTemp(rightSetpoint, r, sizeof(r));
      drawStatusLine(l, r, y, th.secondary);
    }
    else if (!powerOn)
    {
      if (offDetentAccum == 0 && activeSide != SIDE_BOTH)
      {
        formatTemp(currentF, val, sizeof(val));
        drawStatusLine("Off", val, y, th.muted);
      }
      else
      {
        drawStatusLine("Off", "", y, th.muted);
      }
    }
    else if (offDetentAccum > 0)
    {
      drawStatusLine("turn once more for off", "", y, th.secondary);
    }
    else if (activeSide == SIDE_BOTH)
    {
      drawStatusLine("both sides", "", y, th.secondary);
    }
    else if (abs(diff) <= 1)
    {
      formatTemp(currentF, val, sizeof(val));
      drawStatusLine("at", val, y, th.secondary);
    }
    else
    {
      formatTemp(currentF, val, sizeof(val));
      drawStatusLine(diff > 0 ? "heating" : "cooling", val, y, diff > 0 ? th.warm : th.cool);
    }
  }

  // ---- Clock / connectivity at the arc opening ----
  sprite.setTextDatum(middle_center);
  if (!wifiConnected)
  {
    sprite.setFont(&fonts::FreeSans9pt7b);
    sprite.setTextColor(th.alert);
    sprite.drawString("No Wi-Fi", centerX, 204);
    sprite.setFont(&fonts::Font0);
    sprite.setTextColor(th.muted);
    sprite.drawString("hold for settings", centerX, 220);
  }
  else if (timeInitialized)
  {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo))
    {
      lastDrawnMinute = timeinfo.tm_min;
      char timeStr[8];
      snprintf(timeStr, sizeof(timeStr), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
      sprite.setFont(&fonts::Font2);
      sprite.setTextColor(th.secondary);
      sprite.drawString(timeStr, centerX, 206);
    }
  }

  // ---- Settings hold ring (outside the arc) ----
  if (holdActive && !holdConsumed)
  {
    unsigned long held = now - holdStartTime;
    if (held >= HOLD_RING_SHOW_MS)
    {
      float p = (float)(held - HOLD_RING_SHOW_MS) / (float)(SETTINGS_HOLD_MS - HOLD_RING_SHOW_MS);
      if (p > 1.0f) p = 1.0f;
      p = easeOutCubic(p);
      sprite.fillArc(centerX, centerY, 118, 113, 270, 270 + 360.0f * p, th.text);
    }
  }

  sprite.pushSprite(0, 0);
}

// Glanceable state at ~1% backlight: only full-value pixels survive, so a
// single big numeral plus one status dot. No grays, no arc, no clock.
void renderDimScreen()
{
  Theme th = currentTheme();
  sprite.fillSprite(th.bg);

  bool powerOn = isActivePowerOn();
  bool online = wifiConnected && podReachable;
  uint16_t color = th.night ? UI_N_HIGH : UI_TEXT;

  if (powerOn)
  {
    drawTemperatureNumeral(getDisplaySetpoint(), centerX, centerY, &fonts::DejaVu72, color, color, false);
  }
  else
  {
    sprite.setFont(&fonts::FreeSansBold18pt7b);
    sprite.setTextColor(color);
    sprite.setTextDatum(middle_center);
    sprite.drawString("off", centerX, centerY);
  }

  uint16_t dot = 0;
  if (!online) dot = th.night ? UI_N_HIGH : UI_ALERT;
  else if (powerOn && activeSide != SIDE_BOTH)
  {
    int currentF = (activeSide == SIDE_RIGHT) ? rightCurrentTempF : leftCurrentTempF;
    int diff = getDisplaySetpoint() - currentF;
    if (abs(diff) > 1) dot = th.night ? UI_N_HIGH : (diff > 0 ? UI_WARM : UI_COOL);
  }
  if (dot) sprite.fillSmoothCircle(centerX, 176, 3, dot);

  sprite.pushSprite(0, 0);
}


// ==================== Settings Menu ====================

void drawSettingsMenu()
{
  bool nightMode = isNightTime();
  uint16_t bgColor = nightMode ? COLOR_NIGHT_BACKGROUND : COLOR_BACKGROUND;
  uint16_t textColor = nightMode ? COLOR_NIGHT_TEXT : COLOR_TEXT;
  uint16_t accentColor = nightMode ? COLOR_NIGHT_SETPOINT : COLOR_SETPOINT;
  uint16_t dimTextColor = nightMode ? 0x4000 : 0x4208;

  sprite.fillSprite(bgColor);

  sprite.setTextColor(accentColor);
  sprite.setTextDatum(middle_center);
  sprite.setFont(&fonts::FreeSans12pt7b);
  sprite.drawString("Settings", centerX, 25);

  const int centerY_menu = SCREEN_HEIGHT / 2;
  const int itemSpacing = 40;

  for (int i = -2; i <= 2; i++)
  {
    int itemIndex = ((int)currentMenuItem + i + MENU_COUNT) % MENU_COUNT;
    int yPos = centerY_menu + (i * itemSpacing);
    if (yPos < 50 || yPos > SCREEN_HEIGHT - 30) continue;

    MenuItem item = (MenuItem)itemIndex;
    String itemName = getMenuItemName(item);

    if (i == 0)
    {
      sprite.setFont(&fonts::FreeSansBold12pt7b);
      sprite.setTextColor(accentColor);
      sprite.setTextDatum(middle_center);
      sprite.drawString(itemName.c_str(), centerX, yPos);

      // Current value
      sprite.setTextColor(textColor);
      String value = "";
      switch (item)
      {
      case MENU_WIFI_SETTINGS:
        value = wifiConnected ? WiFi.localIP().toString() : "Not connected";
        break;
      case MENU_POD_IP:
        value = podIP.toString() + ":" + String(podPort);
        break;
      case MENU_MDNS_DISCOVER:
        value = podFound ? "Found" : "Tap to scan";
        break;
      case MENU_TEMP_UNIT:
        value = useFahrenheit ? "Fahrenheit" : "Celsius";
        break;
      case MENU_NIGHT_MODE:
        value = nightOverride == NIGHT_AUTO ? "Auto"
                : (nightOverride == NIGHT_FORCE_ON ? "Forced On" : "Forced Off");
        break;
      case MENU_DEFAULT_SIDE:
        value = defaultRightSide ? rightSideName : leftSideName;
        break;
      default: break;
      }
      sprite.setFont(&fonts::Font0);
      sprite.drawString(value.c_str(), centerX, yPos + 18);

      // Selection arrows
      sprite.setFont(&fonts::FreeSans9pt7b);
      sprite.setTextColor(accentColor);
      sprite.drawString(">", centerX - 100, yPos);
      sprite.drawString("<", centerX + 100, yPos);
    }
    else
    {
      sprite.setFont(&fonts::FreeSans9pt7b);
      sprite.setTextColor(dimTextColor);
      sprite.setTextDatum(middle_center);
      sprite.drawString(itemName.c_str(), centerX, yPos);
    }
  }

  // Short hints — the round display clips long lines near the bottom
  sprite.setTextDatum(middle_center);
  sprite.setFont(&fonts::Font0);
  sprite.setTextColor(textColor);
  sprite.drawString("Click: select", centerX, SCREEN_HEIGHT - 24);
  sprite.drawString("Tap: exit", centerX, SCREEN_HEIGHT - 12);

  sprite.pushSprite(0, 0);
}

void handleEncoderInSettings()
{
  long newPosition = M5Dial.Encoder.read();
  long diff = newPosition - lastEncoderPosition;
  static long encoderAccumulator = 0;

  if (diff != 0)
  {
    encoderAccumulator += diff;
    lastEncoderPosition = newPosition;
    recordActivity();

    if (abs(encoderAccumulator) >= 4)
    {
      int steps = encoderAccumulator / 4;
      encoderAccumulator = encoderAccumulator % 4;
      currentMenuItem = (MenuItem)(((int)currentMenuItem + steps + MENU_COUNT) % MENU_COUNT);
      drawSettingsMenu();
    }
  }

  if (M5Dial.BtnA.wasPressed())
  {
    recordActivity();

    switch (currentMenuItem)
    {
    case MENU_WIFI_SETTINGS:
      startWiFiScanner();
      break;
    case MENU_POD_IP:
      startIPEditor();
      break;
    case MENU_MDNS_DISCOVER:
    {
      // Re-run mDNS discovery (blocking — show feedback first)
      drawBusyScreen("Searching...");
      bool nightMode = isNightTime();
      uint16_t scanBg = nightMode ? COLOR_NIGHT_BACKGROUND : COLOR_BACKGROUND;
      uint16_t scanText = nightMode ? COLOR_NIGHT_SETPOINT : COLOR_SETPOINT;
      IPAddress discoveredIP;
      uint16_t discoveredPort;
      if (discoverPod(discoveredIP, discoveredPort))
      {
        podIP = discoveredIP;
        podPort = discoveredPort;
        podFound = true;
        preferences.putUChar("podIP0", podIP[0]);
        preferences.putUChar("podIP1", podIP[1]);
        preferences.putUChar("podIP2", podIP[2]);
        preferences.putUChar("podIP3", podIP[3]);
        preferences.putUShort("podPort", podPort);

        // Show success
        sprite.fillSprite(scanBg);
        sprite.setTextColor(scanText);
        sprite.setFont(&fonts::FreeSans12pt7b);
        sprite.drawString("Found Pod!", centerX, centerY - 15);
        sprite.setFont(&fonts::Font0);
        sprite.drawString((podIP.toString() + ":" + String(podPort)).c_str(), centerX, centerY + 15);
        sprite.pushSprite(0, 0);
        delay(2000);
      }
      else
      {
        // Show not found
        sprite.fillSprite(scanBg);
        sprite.setTextColor(nightMode ? COLOR_NIGHT_ARC_HOT : COLOR_ARC_HOT);
        sprite.setFont(&fonts::FreeSans12pt7b);
        sprite.drawString("Not Found", centerX, centerY - 10);
        sprite.setFont(&fonts::Font0);
        sprite.setTextColor(nightMode ? COLOR_NIGHT_TEXT : COLOR_TEXT);
        sprite.drawString("Set IP manually", centerX, centerY + 15);
        sprite.pushSprite(0, 0);
        delay(2000);
      }
      drawSettingsMenu();
      break;
    }
    case MENU_TEMP_UNIT:
      useFahrenheit = !useFahrenheit;
      unitOverridden = true; // Local choice wins over Pod sync from now on
      preferences.putBool("useFahrenheit", useFahrenheit);
      preferences.putBool("unitOverride", true);
      drawSettingsMenu();
      break;
    case MENU_NIGHT_MODE:
      nightOverride = (NightOverride)(((int)nightOverride + 1) % 3);
      preferences.putUChar("nightOvr", (uint8_t)nightOverride);
      drawSettingsMenu();
      break;
    case MENU_DEFAULT_SIDE:
      // Only changes which side is selected at boot — not the live side
      defaultRightSide = !defaultRightSide;
      preferences.putBool("rightSide", defaultRightSide);
      drawSettingsMenu();
      break;
    default: break;
    }
  }
}

// ==================== IP Editor ====================

void saveIPFromEditor()
{
  podIP = IPAddress(tempIPOctets[0], tempIPOctets[1], tempIPOctets[2], tempIPOctets[3]);
  preferences.putUChar("podIP0", tempIPOctets[0]);
  preferences.putUChar("podIP1", tempIPOctets[1]);
  preferences.putUChar("podIP2", tempIPOctets[2]);
  preferences.putUChar("podIP3", tempIPOctets[3]);
  Serial.printf("Saved Pod IP: %s\n", podIP.toString().c_str());
}

void startIPEditor()
{
  currentSubMenu = SUBMENU_IP_EDITOR;
  ipEditorOctet = 0;
  lastEncoderPosition = M5Dial.Encoder.read();

  for (int i = 0; i < 4; i++)
    tempIPOctets[i] = podIP[i];

  Serial.printf("Editing Pod IP: %d.%d.%d.%d\n",
                tempIPOctets[0], tempIPOctets[1], tempIPOctets[2], tempIPOctets[3]);
  drawIPEditor();
}

void drawIPEditor()
{
  bool nightMode = isNightTime();
  uint16_t bgColor = nightMode ? COLOR_NIGHT_BACKGROUND : COLOR_BACKGROUND;
  uint16_t textColor = nightMode ? COLOR_NIGHT_TEXT : COLOR_TEXT;
  uint16_t accentColor = nightMode ? COLOR_NIGHT_SETPOINT : COLOR_SETPOINT;

  sprite.fillSprite(bgColor);

  sprite.setTextColor(accentColor);
  sprite.setTextDatum(middle_center);
  sprite.setFont(&fonts::FreeSans12pt7b);
  sprite.drawString("Pod IP Address", centerX, centerY - 40);

  sprite.setFont(&fonts::FreeSansBold9pt7b);
  sprite.setTextDatum(middle_center);

  int y = centerY;
  int spacing = 38;
  int startX = centerX - (spacing * 1.5);

  for (int i = 0; i < 4; i++)
  {
    int x = startX + (i * spacing);

    if (i == ipEditorOctet)
    {
      sprite.setTextColor(accentColor);
      sprite.drawRect(x - 16, y - 12, 32, 24, accentColor);
    }
    else
    {
      sprite.setTextColor(textColor);
    }

    char octetStr[4];
    snprintf(octetStr, sizeof(octetStr), "%03d", tempIPOctets[i]);
    sprite.drawString(octetStr, x, y);

    if (i < 3)
    {
      sprite.setTextColor(textColor);
      sprite.drawString(".", x + 19, y);
    }
  }

  sprite.setFont(&fonts::Font0);
  sprite.setTextColor(textColor);
  sprite.setTextDatum(middle_center);
  sprite.drawString("Turn to change | Click for next", centerX, centerY + 35);
  sprite.drawString("Tap to save and exit", centerX, centerY + 50);

  sprite.pushSprite(0, 0);
}

void handleEncoderInIPEditor()
{
  long newPosition = M5Dial.Encoder.read();
  long diff = newPosition - lastEncoderPosition;
  static long encoderAccumulator = 0;

  if (diff != 0)
  {
    encoderAccumulator += diff;
    lastEncoderPosition = newPosition;
    recordActivity();

    if (abs(encoderAccumulator) >= 4)
    {
      int steps = encoderAccumulator / 4;
      encoderAccumulator = encoderAccumulator % 4;

      int newValue = (int)tempIPOctets[ipEditorOctet] + steps;
      if (newValue < 0) newValue = 256 + (newValue % 256);
      if (newValue > 255) newValue = newValue % 256;
      tempIPOctets[ipEditorOctet] = newValue;

      drawIPEditor();
    }
  }

  if (M5Dial.BtnA.wasPressed())
  {
    recordActivity();
    ipEditorOctet++;

    if (ipEditorOctet >= 4)
    {
      saveIPFromEditor();
      currentSubMenu = SUBMENU_NONE;
      lastEncoderPosition = M5Dial.Encoder.read();
      drawSettingsMenu();
    }
    else
    {
      drawIPEditor();
    }
  }
}

// ==================== WiFi Scanner ====================

void startWiFiScanner()
{
  currentSubMenu = SUBMENU_WIFI_SCAN;
  scannedSSIDCount = 0;
  selectedSSIDIndex = 0;
  lastEncoderPosition = M5Dial.Encoder.read();

  // scanNetworks blocks for several seconds — show feedback first
  drawBusyScreen("Scanning...");
  int n = WiFi.scanNetworks();
  scannedSSIDCount = (n > 20) ? 20 : n;

  for (int i = 0; i < scannedSSIDCount; i++)
  {
    scannedSSIDs[i] = WiFi.SSID(i);
  }

  drawWiFiScanner();
}

void drawWiFiScanner()
{
  bool nightMode = isNightTime();
  uint16_t bgColor = nightMode ? COLOR_NIGHT_BACKGROUND : COLOR_BACKGROUND;
  uint16_t textColor = nightMode ? COLOR_NIGHT_TEXT : COLOR_TEXT;
  uint16_t accentColor = nightMode ? COLOR_NIGHT_SETPOINT : COLOR_SETPOINT;
  uint16_t dimTextColor = nightMode ? 0x4000 : 0x4208;

  sprite.fillSprite(bgColor);

  sprite.setTextColor(accentColor);
  sprite.setTextDatum(middle_center);
  sprite.setFont(&fonts::FreeSans12pt7b);
  sprite.drawString("WiFi Networks", centerX, 25);

  if (scannedSSIDCount == 0)
  {
    sprite.setFont(&fonts::FreeSans9pt7b);
    sprite.setTextColor(textColor);
    sprite.drawString("No networks found", centerX, centerY);
    sprite.setFont(&fonts::Font0);
    sprite.drawString("Tap to go back", centerX, SCREEN_HEIGHT - 15);
  }
  else
  {
    const int centerY_menu = SCREEN_HEIGHT / 2;
    const int itemSpacing = 35;

    bool wrap = scannedSSIDCount > 4;

    for (int i = -2; i <= 2; i++)
    {
      int networkIndex = selectedSSIDIndex + i;
      if (wrap)
        networkIndex = (networkIndex % scannedSSIDCount + scannedSSIDCount) % scannedSSIDCount;
      else if (networkIndex < 0 || networkIndex >= scannedSSIDCount)
        continue;

      int yPos = centerY_menu + (i * itemSpacing);
      if (yPos < 50 || yPos > SCREEN_HEIGHT - 30) continue;

      if (i == 0)
      {
        sprite.setFont(&fonts::FreeSansBold12pt7b);
        sprite.setTextColor(accentColor);
        sprite.setTextDatum(middle_center);
        sprite.drawString(fitText(scannedSSIDs[networkIndex], 14).c_str(), centerX, yPos);
        sprite.setFont(&fonts::FreeSans9pt7b);
        sprite.drawString(">", centerX - 100, yPos);
        sprite.drawString("<", centerX + 100, yPos);
      }
      else
      {
        sprite.setFont(&fonts::FreeSans9pt7b);
        sprite.setTextColor(dimTextColor);
        sprite.setTextDatum(middle_center);
        sprite.drawString(fitText(scannedSSIDs[networkIndex], 18).c_str(), centerX, yPos);
      }
    }

    // Short hints — the round display clips long lines near the bottom
    sprite.setFont(&fonts::Font0);
    sprite.setTextColor(textColor);
    sprite.setTextDatum(middle_center);
    sprite.drawString("Click: connect", centerX, SCREEN_HEIGHT - 24);
    sprite.drawString("Tap: back", centerX, SCREEN_HEIGHT - 12);
  }

  sprite.pushSprite(0, 0);
}

void handleEncoderInWiFiScanner()
{
  long newPosition = M5Dial.Encoder.read();
  long diff = newPosition - lastEncoderPosition;
  static long encoderAccumulator = 0;

  if (diff != 0)
  {
    encoderAccumulator += diff;
    lastEncoderPosition = newPosition;
    recordActivity();

    if (abs(encoderAccumulator) >= 4 && scannedSSIDCount > 0)
    {
      int steps = encoderAccumulator / 4;
      encoderAccumulator = encoderAccumulator % 4;

      selectedSSIDIndex += steps;
      if (scannedSSIDCount > 4)
      {
        // Wrap around like the settings menu
        selectedSSIDIndex = (selectedSSIDIndex % scannedSSIDCount + scannedSSIDCount) % scannedSSIDCount;
      }
      else
      {
        if (selectedSSIDIndex < 0) selectedSSIDIndex = 0;
        if (selectedSSIDIndex >= scannedSSIDCount) selectedSSIDIndex = scannedSSIDCount - 1;
      }

      drawWiFiScanner();
    }
  }

  if (M5Dial.BtnA.wasPressed())
  {
    recordActivity();
    if (scannedSSIDCount > 0) startPasswordEntry();
  }
}

// ==================== Password Entry ====================

void startPasswordEntry()
{
  currentSubMenu = SUBMENU_WIFI_PASSWORD;
  wifiPasswordInput = "";
  passwordCharIndex = 0;
  // Start true so the release of the click that opened this screen
  // doesn't append a character; resets on the next press
  pwLongPressFired = true;
  lastEncoderPosition = M5Dial.Encoder.read();
  drawPasswordEntry();
}

void drawPasswordEntry()
{
  bool nightMode = isNightTime();
  uint16_t bgColor = nightMode ? COLOR_NIGHT_BACKGROUND : COLOR_BACKGROUND;
  uint16_t textColor = nightMode ? COLOR_NIGHT_TEXT : COLOR_TEXT;
  uint16_t accentColor = nightMode ? COLOR_NIGHT_SETPOINT : COLOR_SETPOINT;

  sprite.fillSprite(bgColor);

  sprite.setTextColor(accentColor);
  sprite.setTextDatum(middle_center);
  sprite.setFont(&fonts::FreeSans12pt7b);
  sprite.drawString("WiFi Password", centerX, 25);

  sprite.setFont(&fonts::FreeSans9pt7b);
  sprite.setTextColor(textColor);
  sprite.drawString(fitText(scannedSSIDs[selectedSSIDIndex], 18).c_str(), centerX, 52);

  // Masked password (show the tail when it outgrows the screen)
  sprite.setFont(&fonts::FreeSansBold12pt7b);
  sprite.setTextColor(textColor);
  sprite.setTextDatum(middle_center);
  String maskedPassword = "";
  for (unsigned int i = 0; i < wifiPasswordInput.length(); i++) maskedPassword += "*";
  if (maskedPassword.length() > 16)
    maskedPassword = maskedPassword.substring(maskedPassword.length() - 16);
  sprite.drawString(maskedPassword.c_str(), centerX, 82);

  // Character carousel (last entry is a virtual DEL/backspace)
  const int charSpacing = 30;
  const int centerY_char = centerY + 30;
  const int alphaLen = strlen(alphaNumeric);
  const int carouselLen = alphaLen + 1;

  for (int i = -1; i <= 1; i++)
  {
    int charIdx = passwordCharIndex + i;
    charIdx = (charIdx + carouselLen) % carouselLen;
    int yPos = centerY_char + (i * charSpacing);
    bool isDel = (charIdx == alphaLen);

    if (i == 0)
    {
      sprite.setTextColor(accentColor);
      if (isDel)
      {
        sprite.setFont(&fonts::FreeSansBold9pt7b);
        sprite.drawString("DEL", centerX, yPos);
        sprite.drawRect(centerX - 26, yPos - 15, 52, 30, accentColor);
      }
      else
      {
        sprite.setFont(&fonts::FreeSansBold18pt7b);
        char charStr[2] = {alphaNumeric[charIdx], '\0'};
        sprite.drawString(charStr, centerX, yPos);
        sprite.drawRect(centerX - 15, yPos - 18, 30, 36, accentColor);
      }
    }
    else
    {
      sprite.setTextColor(textColor);
      if (isDel)
      {
        sprite.setFont(&fonts::FreeSans9pt7b);
        sprite.drawString("DEL", centerX, yPos);
      }
      else
      {
        sprite.setFont(&fonts::FreeSans12pt7b);
        char charStr[2] = {alphaNumeric[charIdx], '\0'};
        sprite.drawString(charStr, centerX, yPos);
      }
    }
  }

  // Short hints — the round display clips long lines near the bottom
  sprite.setFont(&fonts::Font0);
  sprite.setTextColor(textColor);
  sprite.setTextDatum(middle_center);
  sprite.drawString("Click: add   Hold: connect", centerX, SCREEN_HEIGHT - 34);
  sprite.drawString("Tap: cancel", centerX, SCREEN_HEIGHT - 20);

  sprite.pushSprite(0, 0);
}

void handleEncoderInPasswordEntry()
{
  long newPosition = M5Dial.Encoder.read();
  long diff = newPosition - lastEncoderPosition;
  static long encoderAccumulator = 0;

  if (diff != 0)
  {
    encoderAccumulator += diff;
    lastEncoderPosition = newPosition;
    recordActivity();

    if (abs(encoderAccumulator) >= 4)
    {
      int steps = encoderAccumulator / 4;
      encoderAccumulator = encoderAccumulator % 4;

      int carouselLen = strlen(alphaNumeric) + 1; // +1 for DEL
      passwordCharIndex = ((passwordCharIndex + steps) % carouselLen + carouselLen) % carouselLen;
      drawPasswordEntry();
    }
  }

  // Character add happens on RELEASE of a short press — never on press.
  // Adding on press would append a junk character at the start of every
  // long-press-to-connect.
  if (M5Dial.BtnA.wasPressed())
  {
    recordActivity();
    pwLongPressFired = false;
  }

  // Long press = submit and connect
  if (M5Dial.BtnA.pressedFor(1000) && !pwLongPressFired)
  {
    pwLongPressFired = true;
    recordActivity();

    WiFi.begin(scannedSSIDs[selectedSSIDIndex].c_str(), wifiPasswordInput.c_str());

    bool nightMode = isNightTime();
    uint16_t bgColor = nightMode ? COLOR_NIGHT_BACKGROUND : COLOR_BACKGROUND;
    uint16_t accentColor = nightMode ? COLOR_NIGHT_SETPOINT : COLOR_SETPOINT;

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20)
    {
      sprite.fillSprite(bgColor);
      sprite.setTextColor(accentColor);
      sprite.setTextDatum(middle_center);
      sprite.setFont(&fonts::FreeSans12pt7b);
      sprite.drawString("Connecting", centerX, centerY - 10);
      String dots = "";
      for (int i = 0; i <= (attempts % 3); i++) dots += ".";
      sprite.drawString(dots.c_str(), centerX, centerY + 20);
      sprite.pushSprite(0, 0);
      delay(500);
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
      wifiConnected = true;
      savedWifiSSID = scannedSSIDs[selectedSSIDIndex];
      savedWifiPassword = wifiPasswordInput;
      preferences.putString("wifiSSID", savedWifiSSID);
      preferences.putString("wifiPass", savedWifiPassword);
      if (!networkServicesStarted)
      {
        // Boot was offline — mDNS and NTP never started
        setupMDNS();
        setupNTP();
        networkServicesStarted = true;
      }

      sprite.fillSprite(bgColor);
      sprite.setTextColor(accentColor);
      sprite.setFont(&fonts::FreeSans12pt7b);
      sprite.drawString("Connected!", centerX, centerY);
      sprite.pushSprite(0, 0);
      delay(2000);
    }
    else
    {
      sprite.fillSprite(bgColor);
      sprite.setTextColor(0xF800);
      sprite.setFont(&fonts::FreeSans12pt7b);
      sprite.drawString("Connection Failed", centerX, centerY);
      sprite.pushSprite(0, 0);
      delay(2000);

      // Fall back to the previously saved network so a failed attempt
      // doesn't strand the dial offline
      wifiConnected = false;
      if (savedWifiSSID.length() > 0)
      {
        WiFi.begin(savedWifiSSID.c_str(), savedWifiPassword.c_str());
      }
      else
      {
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      }
    }

    currentSubMenu = SUBMENU_NONE;
    lastEncoderPosition = M5Dial.Encoder.read();
    drawSettingsMenu();
    return;
  }

  // Short-press release = add selected character (or DEL = backspace)
  if (M5Dial.BtnA.wasReleased() && !pwLongPressFired)
  {
    recordActivity();
    int alphaLen = strlen(alphaNumeric);
    if (passwordCharIndex == alphaLen)
    {
      if (wifiPasswordInput.length() > 0)
        wifiPasswordInput.remove(wifiPasswordInput.length() - 1);
    }
    else
    {
      wifiPasswordInput += alphaNumeric[passwordCharIndex];
    }
    drawPasswordEntry();
  }
}


// ==================== Serial Debug ====================

// Single-character commands over USB serial so the UI can be exercised and
// inspected without touching the device:
//   + / -  one detent up / down     c  click (cycle side)   o  toggle power
//   n      cycle night override     z  force dim now        w  wake
//   p      dump the framebuffer as hex (240 rows of RGB565)
void handleSerialDebug()
{
  while (Serial.available())
  {
    char c = Serial.read();
    switch (c)
    {
    case '+': simulatedEncoderDelta += 1; break;
    case '-': simulatedEncoderDelta -= 1; break;
    case 'c': if (!inSettingsMenu) { recordActivity(); cycleSide(+1); } break;
    case 'o': if (!inSettingsMenu) { recordActivity(); setActivePower(!isActivePowerOn()); } break;
    case 'n':
      nightOverride = (NightOverride)(((int)nightOverride + 1) % 3);
      Serial.printf("night override -> %d\n", (int)nightOverride);
      drawTemperatureUI();
      break;
    case 'z': lastActivityTime = millis() - DIM_TIMEOUT_MS - 1; break;
    case 'w': recordActivity(); break;
    case 'p': dumpScreen(); break;
    default: break;
    }
  }
}

void dumpScreen()
{
  const uint16_t *buf = (const uint16_t *)sprite.getBuffer();
  static const char hex[] = "0123456789abcdef";
  char line[SCREEN_WIDTH * 4 + 1];
  Serial.println("<<FB>>");
  for (int y = 0; y < SCREEN_HEIGHT; y++)
  {
    for (int x = 0; x < SCREEN_WIDTH; x++)
    {
      uint16_t v = buf[y * SCREEN_WIDTH + x];
      line[x * 4 + 0] = hex[(v >> 12) & 0xF];
      line[x * 4 + 1] = hex[(v >> 8) & 0xF];
      line[x * 4 + 2] = hex[(v >> 4) & 0xF];
      line[x * 4 + 3] = hex[v & 0xF];
    }
    line[SCREEN_WIDTH * 4] = 0;
    Serial.println(line);
  }
  Serial.println("<<END>>");
}

// ==================== Utility ====================

float mapFloat(float x, float in_min, float in_max, float out_min, float out_max)
{
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// One short, quiet tick for actions with no other immediate feedback (side
// change, off/on stop, settings opening). Rotation is always silent: the
// detents are the feedback. Fully silent at night — a partner 40cm away
// must never hear the device. Errors are never audible; they show on screen.
void feedbackBeep(uint16_t freq)
{
  if (isNightTime()) return;
  M5Dial.Speaker.tone(freq, 10);
}

// Full-screen message for blocking operations (WiFi scan, mDNS discovery)
void drawBusyScreen(const char *msg)
{
  bool nightMode = isNightTime();
  sprite.fillSprite(nightMode ? COLOR_NIGHT_BACKGROUND : COLOR_BACKGROUND);
  sprite.setTextColor(nightMode ? COLOR_NIGHT_SETPOINT : COLOR_SETPOINT);
  sprite.setTextDatum(middle_center);
  sprite.setFont(&fonts::FreeSans12pt7b);
  sprite.drawString(msg, centerX, centerY);
  sprite.pushSprite(0, 0);
}

// Truncate with ellipsis so long strings don't clip off the round display
String fitText(const String &s, unsigned int maxChars)
{
  if (s.length() <= maxChars) return s;
  return s.substring(0, maxChars - 2) + "..";
}


// ==================== Time & Brightness ====================

void setupNTP()
{
  if (!wifiConnected) return;

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo) && attempts < 10)
  {
    delay(500);
    attempts++;
  }

  if (attempts < 10)
  {
    timeInitialized = true;
    Serial.println("Time synchronized!");
    M5Dial.Rtc.setDateTime(&timeinfo);
  }
}

bool isNightTime()
{
  if (nightOverride == NIGHT_FORCE_ON) return true;
  if (nightOverride == NIGHT_FORCE_OFF) return false;
  if (!timeInitialized) return false;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return false;

  int hour = timeinfo.tm_hour;
  if (NIGHT_START_HOUR > NIGHT_END_HOUR)
    return (hour >= NIGHT_START_HOUR || hour < NIGHT_END_HOUR);
  else
    return (hour >= NIGHT_START_HOUR && hour < NIGHT_END_HOUR);
}



void recordActivity()
{
  lastActivityTime = millis();
  if (isDimmed)
  {
    isDimmed = false;
    drawTemperatureUI();
    updateBrightness();
  }
}

// Backlight: dim after DIM_TIMEOUT (shorter at night) and fade between
// levels instead of stepping. The fade runs in perceptual (sqrt) space so
// the low end doesn't collapse in the first few milliseconds.
void updateBrightness()
{
  unsigned long now = millis();
  unsigned long timeSinceActivity = now - lastActivityTime;
  unsigned long dimTimeout = isNightTime() ? DIM_TIMEOUT_NIGHT_MS : DIM_TIMEOUT_MS;
  uint8_t targetBrightness;

  if (!inSettingsMenu && timeSinceActivity > dimTimeout)
  {
    targetBrightness = BRIGHTNESS_DIM;
    if (!isDimmed)
    {
      isDimmed = true;
      dimmedAt = now;
      holdActive = false;
      drawTemperatureUI();
    }
  }
  else
  {
    targetBrightness = isNightTime() ? BRIGHTNESS_NIGHT : BRIGHTNESS_DAY;
    isDimmed = false;
  }

  if ((float)targetBrightness != brightnessTarget)
  {
    unsigned long dur = targetBrightness > brightnessNow ? WAKE_FADE_MS : SLEEP_FADE_MS;
    brightnessTarget = (float)targetBrightness;
    tweenStart(brightnessTween, sqrtf(brightnessNow), sqrtf(brightnessTarget), dur, now);
  }

  float s = tweenValue(brightnessTween, now);
  float v = s * s;
  if ((int)v != (int)brightnessNow)
  {
    M5Dial.Display.setBrightness((uint8_t)v);
  }
  brightnessNow = v;
}

// ==================== State Helpers ====================

int &getActiveSetpoint()
{
  return activeSide == SIDE_RIGHT ? rightSetpoint : leftSetpoint;
}

// What the numeral shows: the active side's setpoint, or the carried value
// in Both mode until the sides are pulled together
int getDisplaySetpoint()
{
  if (activeSide == SIDE_BOTH) return bothDisplayF;
  return getActiveSetpoint();
}

bool isActivePowerOn()
{
  if (activeSide == SIDE_LEFT) return leftPowerOn;
  if (activeSide == SIDE_RIGHT) return rightPowerOn;
  return leftPowerOn || rightPowerOn;
}

String getMenuItemName(MenuItem item)
{
  switch (item)
  {
  case MENU_WIFI_SETTINGS:  return "WiFi Settings";
  case MENU_POD_IP:         return "Pod IP Address";
  case MENU_MDNS_DISCOVER:  return "Discover Pod";
  case MENU_TEMP_UNIT:      return "Temperature Unit";
  case MENU_NIGHT_MODE:     return "Night Mode";
  case MENU_DEFAULT_SIDE:   return "Default Side";
  default:                  return "Unknown";
  }
}

// ==================== Pod Sync ====================

// Record the result of a Pod HTTP request and return whether the Pod
// should be shown as reachable. Every failure feeds the back-off
// counter; "Pod offline" only shows after two consecutive failures so
// a single transient miss doesn't flap the banner.
bool notePodRequestResult(bool ok)
{
  if (ok) podSyncFailures = 0;
  else podSyncFailures++;
  return ok || podSyncFailures < 2;
}

// Send whatever changed since the last flush: power first (so a setpoint
// lands on a side that is on), then temperature, per side.
void flushPendingApi()
{
  pendingApiUpdate = false;
  bool ok = true;
  const char *names[2] = {"left", "right"};
  int *setpoints[2] = {&leftSetpoint, &rightSetpoint};
  bool *powers[2] = {&leftPowerOn, &rightPowerOn};

  for (int i = 0; i < 2; i++)
  {
    if (pendingPower[i])
    {
      pendingPower[i] = false;
      ok = setPodPower(podIP, names[i], *powers[i], podPort) && ok;
    }
    if (pendingTemp[i])
    {
      pendingTemp[i] = false;
      ok = setPodTemperature(podIP, names[i], *setpoints[i], podPort) && ok;
    }
  }

  podReachable = notePodRequestResult(ok);
  drawTemperatureUI(); // cap turns solid (or "Pod offline" appears)
}


void syncStatusFromPod()
{
  Serial.println("Syncing status from Pod...");

  // Fetch device status (temperatures, power)
  PodStatus status = fetchPodStatus(podIP, podPort);
  podReachable = notePodRequestResult(status.success);
  if (status.success)
  {
    if (status.left.valid)
    {
      if (status.left.targetTemperatureF >= TEMP_MIN_F && status.left.targetTemperatureF <= TEMP_MAX_F)
        leftSetpoint = status.left.targetTemperatureF;
      leftCurrentTempF = status.left.currentTemperatureF;
      leftPowerOn = status.left.isPowered;
      Serial.printf("Left synced: target=%d°F actual=%d°F %s\n", leftSetpoint, leftCurrentTempF, leftPowerOn ? "ON" : "OFF");
    }
    if (status.right.valid)
    {
      if (status.right.targetTemperatureF >= TEMP_MIN_F && status.right.targetTemperatureF <= TEMP_MAX_F)
        rightSetpoint = status.right.targetTemperatureF;
      rightCurrentTempF = status.right.currentTemperatureF;
      rightPowerOn = status.right.isPowered;
      Serial.printf("Right synced: target=%d°F actual=%d°F %s\n", rightSetpoint, rightCurrentTempF, rightPowerOn ? "ON" : "OFF");
    }
  }

  // Fetch settings (side names, temp unit, reboot schedule)
  PodSettings settings = fetchPodSettings(podIP, podPort);
  if (settings.success)
  {
    // Update side names
    if (settings.leftName.length() > 0)
    {
      leftSideName = settings.leftName;
      preferences.putString("leftName", leftSideName);
    }
    if (settings.rightName.length() > 0)
    {
      rightSideName = settings.rightName;
      preferences.putString("rightName", rightSideName);
    }

    // Sync temperature unit preference from Pod — unless the user has
    // explicitly chosen a unit on the dial
    bool podUsesF = (settings.temperatureUnit == "F");
    if (!unitOverridden && podUsesF != useFahrenheit)
    {
      useFahrenheit = podUsesF;
      preferences.putBool("useFahrenheit", useFahrenheit);
      Serial.printf("Synced temp unit from Pod: %s\n", useFahrenheit ? "°F" : "°C");
    }

    // Sync auto-restart setting from Pod
    autoRestartEnabled = settings.rebootDaily;
    if (settings.rebootTime.length() >= 4)
    {
      autoRestartHour = settings.rebootTime.substring(0, 2).toInt();
    }
    Serial.printf("Auto-restart: %s at %02d:00\n",
                  autoRestartEnabled ? "enabled" : "disabled", autoRestartHour);
  }
}



void syncFromPod()
{
  PodStatus status = fetchPodStatus(podIP, podPort);
  bool needsRedraw = false;

  bool reachable = notePodRequestResult(status.success);
  if (reachable != podReachable)
  {
    podReachable = reachable;
    needsRedraw = true;
  }

  if (status.success)
  {
    if (status.left.valid)
    {
      if (leftPowerOn != status.left.isPowered) { leftPowerOn = status.left.isPowered; needsRedraw = true; }
      // The Pod reports target 0 for a side that is off; keep the last real setpoint
      if (leftSetpoint != status.left.targetTemperatureF &&
          status.left.targetTemperatureF >= TEMP_MIN_F && status.left.targetTemperatureF <= TEMP_MAX_F)
      { leftSetpoint = status.left.targetTemperatureF; needsRedraw = true; }
      if (leftCurrentTempF != status.left.currentTemperatureF) { leftCurrentTempF = status.left.currentTemperatureF; needsRedraw = true; }
    }
    if (status.right.valid)
    {
      if (rightPowerOn != status.right.isPowered) { rightPowerOn = status.right.isPowered; needsRedraw = true; }
      if (rightSetpoint != status.right.targetTemperatureF &&
          status.right.targetTemperatureF >= TEMP_MIN_F && status.right.targetTemperatureF <= TEMP_MAX_F)
      { rightSetpoint = status.right.targetTemperatureF; needsRedraw = true; }
      if (rightCurrentTempF != status.right.currentTemperatureF) { rightCurrentTempF = status.right.currentTemperatureF; needsRedraw = true; }
    }
    if (activeSide == SIDE_BOTH && leftSetpoint == rightSetpoint) bothDisplayF = leftSetpoint;
  }

  if (needsRedraw) drawTemperatureUI();
}
