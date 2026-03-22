// Sleepypod MT Rotary Dial — M5Stack Dial temperature controller for sleepypod-core
//
// Based on RotaryDial by dallonby (https://github.com/dallonby/RotaryDial)
// Adapted to use sleepypod-core tRPC/REST APIs instead of FreeSleep
//
// Controls left/right sides of an Eight Sleep Pod via sleepypod-core,
// with mDNS auto-discovery, rotary dial interface, and automatic night mode.

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
unsigned long lastClockUpdate = 0;
bool isDimmed = false;
bool timeInitialized = false;
bool rightSideActive = false;   // false = left side (default), true = right side
bool nightModeOverride = false; // Manual night mode override
bool inSettingsMenu = false;

// Saved WiFi credentials
String savedWifiSSID = "";
String savedWifiPassword = "";

// Temperature unit setting (true = Fahrenheit, false = Celsius)
bool useFahrenheit = true; // Default to Fahrenheit (native API unit)

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

// Debounce for sleepypod-core API updates
unsigned long lastSetpointChangeTime = 0;
bool pendingApiUpdate = false;
const unsigned long API_DEBOUNCE_MS = 500;

// Periodic sync from sleepypod-core
unsigned long lastPodSync = 0;
const unsigned long POD_SYNC_INTERVAL_MS = 30000; // 30 seconds

// Track night mode state to detect changes
bool wasNightMode = false;

// Touch duration tracking for center tap
unsigned long centerTouchStartTime = 0;
unsigned long lastCenterTapTime = 0;
bool centerTouchActive = false;
bool waitingForDoubleClick = false;
const unsigned long CLICK_MAX_MS = 400;
const unsigned long NIGHT_MODE_MAX_MS = 1000;
const unsigned long DOUBLE_CLICK_WINDOW_MS = 500;

// Encoder button double-click tracking
unsigned long lastEncoderButtonTime = 0;
bool waitingForEncoderDoubleClick = false;

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
const char alphaNumeric[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!@#$%^&*()_+-=[]{}|;:',.<>?/ ";

// Display dimensions
const int centerX = SCREEN_WIDTH / 2;
const int centerY = SCREEN_HEIGHT / 2;
const int arcRadius = 100;
const int arcThickness = 20;

// Sprite for double buffering
LGFX_Sprite sprite(&M5Dial.Display);

// ==================== Function Prototypes ====================

void setupWiFi();
void setupMDNS();
void setupNTP();
void drawTemperatureUI();
void drawSettingsMenu();
void drawIPEditor();
void drawWiFiScanner();
void drawPasswordEntry();
void updateClockDisplay();
void handleEncoderInput();
void handleEncoderInSettings();
void handleEncoderInIPEditor();
void handleEncoderInWiFiScanner();
void handleEncoderInPasswordEntry();
void handleTouchInput();
void updateBrightness();
void recordActivity();
bool isNightTime();
uint16_t getTemperatureColor(float percent);
uint16_t getTemperatureColorNight(float percent);
float mapFloat(float x, float in_min, float in_max, float out_min, float out_max);
int &getActiveSetpoint();
int &getInactiveSetpoint();
String getMenuItemName(MenuItem item);
void startIPEditor();
void startWiFiScanner();
void startPasswordEntry();
void syncFromPod();
void syncStatusFromPod();
void toggleActivePower();

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

  // Load default side
  rightSideActive = preferences.getBool("rightSide", false);
  Serial.printf("Default side: %s\n", rightSideActive ? "Right" : "Left");

  // Load cached side names (updated from Pod settings on sync)
  leftSideName = preferences.getString("leftName", "Left");
  rightSideName = preferences.getString("rightName", "Right");
  Serial.printf("Side names: L='%s' R='%s'\n", leftSideName.c_str(), rightSideName.c_str());

  // Initialize display
  M5Dial.Display.setRotation(0);
  M5Dial.Display.fillScreen(COLOR_BACKGROUND);
  M5Dial.Display.setTextColor(COLOR_TEXT);
  M5Dial.Display.setTextDatum(middle_center);

  // Create sprite for double buffering
  sprite.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT);

  // Show startup message
  M5Dial.Display.setTextSize(1);
  M5Dial.Display.drawString("Connecting...", centerX, centerY);

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
  }

  // Get initial encoder position
  lastEncoderPosition = M5Dial.Encoder.read();
  lastActivityTime = millis();
  recordActivity();
  wasNightMode = isNightTime();

  drawTemperatureUI();
}

// ==================== Main Loop ====================

void loop()
{
  M5Dial.update();
  unsigned long currentMillis = millis();

  handleEncoderInput();
  handleTouchInput();

  // Check for single-click action after double-click window expires
  if (waitingForDoubleClick && (currentMillis - lastCenterTapTime >= DOUBLE_CLICK_WINDOW_MS))
  {
    waitingForDoubleClick = false;
    Serial.println("Single-click confirmed - toggling power");
    toggleActivePower();
  }

  // Check for single encoder button click after double-click window
  if (waitingForEncoderDoubleClick && (currentMillis - lastEncoderButtonTime >= DOUBLE_CLICK_WINDOW_MS))
  {
    waitingForEncoderDoubleClick = false;
    if (!inSettingsMenu)
    {
      Serial.println("Encoder single-click - toggling power");
      toggleActivePower();
    }
  }

  updateBrightness();

  // Update clock every second (main screen only)
  if (!inSettingsMenu && currentMillis - lastClockUpdate >= 1000)
  {
    lastClockUpdate = currentMillis;
    updateClockDisplay();
  }

  // Handle debounced API updates
  if (pendingApiUpdate && (currentMillis - lastSetpointChangeTime >= API_DEBOUNCE_MS))
  {
    pendingApiUpdate = false;
    const char *side = rightSideActive ? "right" : "left";
    setPodTemperature(podIP, side, getActiveSetpoint(), podPort);
  }

  // Periodic sync from Pod
  if (wifiConnected && !inSettingsMenu && !pendingApiUpdate &&
      (currentMillis - lastPodSync >= POD_SYNC_INTERVAL_MS))
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

  delay(2);
}

// ==================== WiFi & Network ====================

void setupWiFi()
{
  const char *ssid = savedWifiSSID.length() > 0 ? savedWifiSSID.c_str() : WIFI_SSID;
  const char *password = savedWifiPassword.length() > 0 ? savedWifiPassword.c_str() : WIFI_PASSWORD;

  Serial.printf("Connecting to WiFi: %s\n", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30)
  {
    delay(500);
    Serial.print(".");
    attempts++;

    M5Dial.Display.fillScreen(COLOR_BACKGROUND);
    M5Dial.Display.setTextSize(1);
    M5Dial.Display.drawString("Connecting to WiFi", centerX, centerY - 20);
    String dots = "";
    for (int i = 0; i < (attempts % 4); i++)
      dots += ".";
    M5Dial.Display.drawString(dots.c_str(), centerX, centerY + 10);
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    wifiConnected = true;
    Serial.printf("\nWiFi Connected! IP: %s\n", WiFi.localIP().toString().c_str());

    M5Dial.Display.fillScreen(COLOR_BACKGROUND);
    M5Dial.Display.setTextColor(COLOR_SETPOINT);
    M5Dial.Display.drawString("WiFi Connected!", centerX, centerY - 20);
    M5Dial.Display.setTextColor(COLOR_TEXT);
    M5Dial.Display.drawString(WiFi.localIP().toString().c_str(), centerX, centerY + 10);
    delay(1500);
  }
  else
  {
    wifiConnected = false;
    Serial.println("\nWiFi Failed!");

    M5Dial.Display.fillScreen(COLOR_BACKGROUND);
    M5Dial.Display.setTextColor(COLOR_ARC_HOT);
    M5Dial.Display.drawString("WiFi Failed!", centerX, centerY - 10);
    M5Dial.Display.setTextColor(COLOR_TEXT);
    M5Dial.Display.drawString("Running offline", centerX, centerY + 10);
    delay(2000);
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

// ==================== Web Server (Local API) ====================

// ==================== Encoder Input ====================

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
  static long encoderAccumulator = 0;

  if (diff != 0)
  {
    encoderAccumulator += diff;
    lastEncoderPosition = newPosition;
    recordActivity();

    // 1 encoder count = 1°F step (maximum responsiveness)
    if (abs(encoderAccumulator) >= 1)
    {
      int steps = encoderAccumulator;
      encoderAccumulator = 0;

      int &activeSetpoint = getActiveSetpoint();
      int newTemp = activeSetpoint + steps;

      if (newTemp < TEMP_MIN_F) newTemp = TEMP_MIN_F;
      if (newTemp > TEMP_MAX_F) newTemp = TEMP_MAX_F;

      if (newTemp != activeSetpoint)
      {
        activeSetpoint = newTemp;

        if (useFahrenheit)
        {
          Serial.printf("Encoder: %s %d°F\n",
                        rightSideActive ? "Right" : "Left", activeSetpoint);
        }
        else
        {
          Serial.printf("Encoder: %s %.1f°C\n",
                        rightSideActive ? "Right" : "Left", fahrenheitToCelsius(activeSetpoint));
        }
        drawTemperatureUI();

        // Schedule debounced API update
        lastSetpointChangeTime = millis();
        pendingApiUpdate = true;
      }
    }
  }

  // Encoder button: single-click = power toggle, double-click = reset to default
  if (M5Dial.BtnA.wasPressed())
  {
    unsigned long now = millis();

    if (waitingForEncoderDoubleClick && (now - lastEncoderButtonTime < DOUBLE_CLICK_WINDOW_MS))
    {
      waitingForEncoderDoubleClick = false;
      getActiveSetpoint() = TEMP_DEFAULT_F;
      Serial.printf("Double-click: Reset %s to %d°F\n",
                    rightSideActive ? "Right" : "Left", TEMP_DEFAULT_F);
      recordActivity();
      drawTemperatureUI();
      lastSetpointChangeTime = millis();
      pendingApiUpdate = true;
    }
    else
    {
      bool wasDimmed = isDimmed;
      recordActivity();
      if (wasDimmed)
      {
        Serial.println("Encoder button woke screen");
      }
      else
      {
        waitingForEncoderDoubleClick = true;
        lastEncoderButtonTime = now;
      }
    }
  }
}

// ==================== Touch Input ====================

void handleTouchInput()
{
  auto touch = M5Dial.Touch.getDetail();

  if (touch.wasPressed())
  {
    bool isCenterTouch = !inSettingsMenu &&
                         abs(touch.x - centerX) < 60 &&
                         abs(touch.y - centerY) < 60;

    if (!isCenterTouch)
    {
      recordActivity();
    }

    // Settings menu touch handling
    if (inSettingsMenu)
    {
      if (currentSubMenu != SUBMENU_NONE)
      {
        currentSubMenu = SUBMENU_NONE;
        lastEncoderPosition = M5Dial.Encoder.read();
        drawSettingsMenu();
        return;
      }
      else
      {
        inSettingsMenu = false;
        drawTemperatureUI();
        return;
      }
    }

    // Center touch — wait for release to determine action
    if (abs(touch.x - centerX) < 60 && abs(touch.y - centerY) < 60)
    {
      centerTouchStartTime = millis();
      centerTouchActive = true;
      return;
    }

    // Bottom area — open settings
    if (abs(touch.x - centerX) < 60 && touch.y > SCREEN_HEIGHT - 45)
    {
      inSettingsMenu = true;
      lastEncoderPosition = M5Dial.Encoder.read();
      drawSettingsMenu();
      return;
    }

    // Bottom buttons
    const int buttonY = SCREEN_HEIGHT - 55;
    const int buttonSize = 40;
    const int leftButtonX = 50;
    const int rightButtonX = SCREEN_WIDTH - 50;

    // Left button (active side label) — tap to toggle side
    if (abs(touch.x - leftButtonX) < buttonSize / 2 && abs(touch.y - buttonY) < buttonSize / 2)
    {
      rightSideActive = !rightSideActive;
      Serial.printf("Switched to %s side\n", rightSideActive ? "Right" : "Left");
      drawTemperatureUI();
      return;
    }

    // Right button (gear icon) — open settings
    if (abs(touch.x - rightButtonX) < buttonSize / 2 && abs(touch.y - buttonY) < buttonSize / 2)
    {
      inSettingsMenu = true;
      lastEncoderPosition = M5Dial.Encoder.read();
      Serial.println("Gear button: opened settings");
      drawSettingsMenu();
      return;
    }

    // Temperature arc touch
    int dx = touch.x - centerX;
    int dy = touch.y - centerY;
    float distance = sqrt(dx * dx + dy * dy);

    if (distance > arcRadius - arcThickness - 10 && distance < arcRadius + 30)
    {
      float angle = atan2(dy, dx) * 180.0 / PI;
      if (angle < 0) angle += 360;

      int newTemp;
      if (angle >= 165 && angle <= 360)
      {
        newTemp = (int)round(mapFloat(angle, 165, 375, TEMP_MIN_F, TEMP_MAX_F));
      }
      else if (angle >= 0 && angle <= 15)
      {
        float normalizedAngle = angle + 360;
        newTemp = (int)round(mapFloat(normalizedAngle, 165, 375, TEMP_MIN_F, TEMP_MAX_F));
      }
      else
      {
        return;
      }

      if (newTemp < TEMP_MIN_F) newTemp = TEMP_MIN_F;
      if (newTemp > TEMP_MAX_F) newTemp = TEMP_MAX_F;

      getActiveSetpoint() = newTemp;
      Serial.printf("Touch set %s: %d°F\n",
                    rightSideActive ? "Right" : "Left", getActiveSetpoint());
      drawTemperatureUI();

      lastSetpointChangeTime = millis();
      pendingApiUpdate = true;
    }
  }

  // Touch release — center area duration-based actions
  if (touch.wasReleased() && centerTouchActive)
  {
    centerTouchActive = false;
    unsigned long now = millis();
    unsigned long touchDuration = now - centerTouchStartTime;

    if (touchDuration < CLICK_MAX_MS)
    {
      if (waitingForDoubleClick && (now - lastCenterTapTime < DOUBLE_CLICK_WINDOW_MS))
      {
        // Double-click: reset setpoint
        waitingForDoubleClick = false;
        getActiveSetpoint() = TEMP_DEFAULT_F;
        Serial.printf("Double-click: Reset %s to %d°F\n",
                      rightSideActive ? "Right" : "Left", TEMP_DEFAULT_F);
        if (isDimmed) { isDimmed = false; lastActivityTime = millis(); }
        drawTemperatureUI();
        lastSetpointChangeTime = millis();
        pendingApiUpdate = true;
      }
      else
      {
        // First click — wait for possible double-click
        waitingForDoubleClick = true;
        lastCenterTapTime = now;
        if (isDimmed)
        {
          isDimmed = false;
          lastActivityTime = millis();
          updateBrightness();
          waitingForDoubleClick = false; // Don't toggle power when waking
        }
      }
    }
    else if (touchDuration < NIGHT_MODE_MAX_MS)
    {
      // Medium hold: toggle night mode
      waitingForDoubleClick = false;
      nightModeOverride = !nightModeOverride;
      Serial.printf("Night mode override: %s\n", nightModeOverride ? "ON" : "OFF");
      drawTemperatureUI();
    }
    else
    {
      // Long hold: open settings
      waitingForDoubleClick = false;
      inSettingsMenu = true;
      currentMenuItem = MENU_WIFI_SETTINGS;
      currentSubMenu = SUBMENU_NONE;
      drawSettingsMenu();
    }
  }
}

// ==================== Display ====================

void drawTemperatureUI()
{
  bool nightMode = isNightTime();

  uint16_t bgColor = nightMode ? COLOR_NIGHT_BACKGROUND : COLOR_BACKGROUND;
  uint16_t arcBgColor = nightMode ? COLOR_NIGHT_ARC_BG : COLOR_ARC_BG;
  uint16_t textColor = nightMode ? COLOR_NIGHT_TEXT : COLOR_TEXT;
  uint16_t setpointColor = nightMode ? COLOR_NIGHT_SETPOINT : COLOR_SETPOINT;

  sprite.fillSprite(bgColor);

  // Arc geometry
  const float startAngle = 165.0f;
  const float endAngle = 375.0f;
  const float totalArcDegrees = endAngle - startAngle; // 210 degrees
  const int arcMidRadius = arcRadius - arcThickness / 2;
  const int capRadius = arcThickness / 2;

  // Background arc
  sprite.fillArc(centerX, centerY, arcRadius, arcRadius - arcThickness, startAngle, endAngle, arcBgColor);

  // Colored gradient arc up to current setpoint (small segments for smooth gradient)
  int activeTemp = getActiveSetpoint();
  float tempPercent = (float)(activeTemp - TEMP_MIN_F) / (float)(TEMP_MAX_F - TEMP_MIN_F);
  float currentAngle = startAngle + tempPercent * totalArcDegrees;

  for (float angle = startAngle; angle < currentAngle; angle += 3.0f)
  {
    float segEnd = angle + 3.5f;
    if (segEnd > currentAngle) segEnd = currentAngle + 0.5f;

    float arcPercent = (angle - startAngle) / totalArcDegrees;
    uint16_t color;
    if (nightMode)
      color = getTemperatureColorNight(arcPercent);
    else
      color = getTemperatureColor(arcPercent);

    sprite.fillArc(centerX, centerY, arcRadius, arcRadius - arcThickness, angle, segEnd, color);
  }

  // Rounded end caps (anti-aliased)
  float startRad = fmodf(startAngle, 360.0f) * PI / 180.0f;
  int startCapX = centerX + cos(startRad) * arcMidRadius;
  int startCapY = centerY + sin(startRad) * arcMidRadius;
  uint16_t startColor = nightMode ? getTemperatureColorNight(0.0f) : getTemperatureColor(0.0f);
  sprite.fillSmoothCircle(startCapX, startCapY, capRadius, startColor);

  float endRad = fmodf(currentAngle, 360.0f) * PI / 180.0f;
  int endCapX = centerX + cos(endRad) * arcMidRadius;
  int endCapY = centerY + sin(endRad) * arcMidRadius;
  uint16_t endColor = nightMode ? getTemperatureColorNight(tempPercent) : getTemperatureColor(tempPercent);
  sprite.fillSmoothCircle(endCapX, endCapY, capRadius, endColor);

  // Active setpoint indicator line
  {
    int lineInner = arcRadius - arcThickness - 3;
    int lineOuter = arcRadius + 3;
    uint16_t lineColor = nightMode ? COLOR_NIGHT_TEXT : COLOR_TEXT;
    int lx1 = centerX + cos(endRad) * lineInner;
    int ly1 = centerY + sin(endRad) * lineInner;
    int lx2 = centerX + cos(endRad) * lineOuter;
    int ly2 = centerY + sin(endRad) * lineOuter;
    sprite.drawLine(lx1, ly1, lx2, ly2, lineColor);
    float offsetRad = endRad + 0.008f;
    sprite.drawLine(
        centerX + (int)(cos(offsetRad) * lineInner),
        centerY + (int)(sin(offsetRad) * lineInner),
        centerX + (int)(cos(offsetRad) * lineOuter),
        centerY + (int)(sin(offsetRad) * lineOuter), lineColor);
  }

  // Power state of active side
  bool activePowerOn = rightSideActive ? rightPowerOn : leftPowerOn;

  // Temperature value in center
  sprite.setTextColor(activePowerOn ? textColor : arcBgColor);
  sprite.setTextDatum(middle_center);
  sprite.setFont(&fonts::FreeSansBold24pt7b);

  char tempStr[10];
  if (useFahrenheit)
  {
    snprintf(tempStr, sizeof(tempStr), "%d", activeTemp);
  }
  else
  {
    float tempC = fahrenheitToCelsius((float)activeTemp);
    snprintf(tempStr, sizeof(tempStr), "%.1f", tempC);
  }

  int tempTextWidth = sprite.textWidth(tempStr);
  sprite.drawString(tempStr, centerX, centerY - 10);

  // Unit indicator (°F / °C)
  int unitX = centerX + tempTextWidth / 2 + 4;
  int unitY = centerY - 20;
  uint16_t unitColor = nightMode ? 0x4000 : 0x6B4D;
  sprite.drawCircle(unitX + 2, unitY - 4, 3, activePowerOn ? unitColor : arcBgColor);
  sprite.setFont(&fonts::FreeSans12pt7b);
  sprite.setTextColor(activePowerOn ? unitColor : arcBgColor);
  sprite.setTextDatum(middle_left);
  sprite.drawString(useFahrenheit ? "F" : "C", unitX + 7, unitY + 4);
  sprite.setTextDatum(middle_center);

  // Status text below target — show heating/cooling direction, hide when at target
  if (activePowerOn)
  {
    int activeCurrentF = rightSideActive ? rightCurrentTempF : leftCurrentTempF;
    int diff = activeTemp - activeCurrentF;

    if (diff != 0)
    {
      sprite.setFont(&fonts::FreeSans9pt7b);
      uint16_t statusColor = nightMode ? 0x4000 : 0x6B4D;
      sprite.setTextColor(statusColor);
      sprite.setTextDatum(middle_center);

      sprite.drawString((diff > 0) ? "heating" : "cooling", centerX, centerY + 25);
    }
  }

  // OFF indicator
  if (!activePowerOn)
  {
    sprite.setFont(&fonts::FreeSansBold12pt7b);
    sprite.setTextColor(nightMode ? COLOR_NIGHT_ARC_HOT : COLOR_ARC_HOT);
    sprite.drawString("OFF", centerX, centerY + 25);
  }

  // Connection status / clock at bottom
  sprite.setFont(&fonts::Font0);
  sprite.setTextDatum(middle_center);

  if (!wifiConnected)
  {
    // Show WiFi disconnected prominently
    sprite.setTextColor(nightMode ? COLOR_NIGHT_ARC_HOT : COLOR_ARC_HOT);
    sprite.drawString("WiFi Disconnected", centerX, SCREEN_HEIGHT - 28);
    sprite.setTextColor(textColor);
    sprite.drawString("Open settings to connect", centerX, SCREEN_HEIGHT - 16);
  }
  else if (timeInitialized)
  {
    sprite.setTextColor(textColor);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo))
    {
      char timeStr[10];
      snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d",
               timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      sprite.drawString(timeStr, centerX, SCREEN_HEIGHT - 22);
    }
  }

  // Bottom buttons
  const int buttonY = SCREEN_HEIGHT - 55;
  const int leftButtonX = 50;
  const int rightButtonX = SCREEN_WIDTH - 50;
  const int btnRadius = 18;

  // Left button — active side initial
  String &activeName = rightSideActive ? rightSideName : leftSideName;
  sprite.fillSmoothCircle(leftButtonX, buttonY, btnRadius, setpointColor);
  sprite.setTextColor(bgColor);
  sprite.setTextDatum(middle_center);
  {
    String label = activeName.substring(0, 2);
    sprite.setFont(label.length() <= 1 ? &fonts::FreeSansBold12pt7b : &fonts::FreeSans9pt7b);
    sprite.drawString(label.c_str(), leftButtonX, buttonY);
  }

  // Right button — gear icon (settings)
  sprite.fillSmoothCircle(rightButtonX, buttonY, btnRadius, arcBgColor);
  {
    // Draw 3 horizontal bars (hamburger/settings icon)
    uint16_t gearColor = textColor;
    int bw = 12; // bar width
    int bh = 2;  // bar height
    int gap = 5; // gap between bars
    for (int i = -1; i <= 1; i++)
    {
      sprite.fillRect(rightButtonX - bw / 2, buttonY + i * gap - bh / 2, bw, bh, gearColor);
    }
  }

  // Push to display
  sprite.pushSprite(0, 0);
}

void updateClockDisplay()
{
  bool nightMode = isNightTime();
  uint16_t bgColor = nightMode ? COLOR_NIGHT_BACKGROUND : COLOR_BACKGROUND;
  uint16_t textColor = nightMode ? COLOR_NIGHT_TEXT : COLOR_TEXT;

  LGFX_Sprite timeSprite(&M5Dial.Display);
  const int timeWidth = 80;
  const int timeHeight = 15;
  const int timeX = centerX - timeWidth / 2;
  const int timeY = SCREEN_HEIGHT - 22 - (timeHeight / 2);

  timeSprite.createSprite(timeWidth, timeHeight);
  timeSprite.fillSprite(bgColor);

  if (timeInitialized)
  {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo))
    {
      timeSprite.setFont(&fonts::Font0);
      timeSprite.setTextColor(textColor);
      timeSprite.setTextDatum(middle_center);

      char timeStr[10];
      snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d",
               timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      timeSprite.drawString(timeStr, timeWidth / 2, timeHeight / 2);
    }
  }

  timeSprite.pushSprite(timeX, timeY);
  timeSprite.deleteSprite();
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
        value = nightModeOverride ? "Override ON" : "Auto";
        break;
      case MENU_DEFAULT_SIDE:
        value = rightSideActive ? rightSideName : leftSideName;
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

  sprite.setTextDatum(middle_center);
  sprite.setFont(&fonts::Font0);
  sprite.setTextColor(textColor);
  sprite.drawString("Turn to navigate | Click to select | Tap to exit", centerX, SCREEN_HEIGHT - 10);

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
      bool nightMode = isNightTime();
      uint16_t scanBg = nightMode ? COLOR_NIGHT_BACKGROUND : COLOR_BACKGROUND;
      uint16_t scanText = nightMode ? COLOR_NIGHT_SETPOINT : COLOR_SETPOINT;

      // Show scanning feedback
      sprite.fillSprite(scanBg);
      sprite.setTextColor(scanText);
      sprite.setTextDatum(middle_center);
      sprite.setFont(&fonts::FreeSans12pt7b);
      sprite.drawString("Scanning...", centerX, centerY);
      sprite.pushSprite(0, 0);

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
      preferences.putBool("useFahrenheit", useFahrenheit);
      drawSettingsMenu();
      break;
    case MENU_NIGHT_MODE:
      nightModeOverride = !nightModeOverride;
      drawSettingsMenu();
      break;
    case MENU_DEFAULT_SIDE:
      rightSideActive = !rightSideActive;
      preferences.putBool("rightSide", rightSideActive);
      drawSettingsMenu();
      break;
    default: break;
    }
  }
}

// ==================== IP Editor ====================

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
      // Save IP
      podIP = IPAddress(tempIPOctets[0], tempIPOctets[1], tempIPOctets[2], tempIPOctets[3]);
      preferences.putUChar("podIP0", tempIPOctets[0]);
      preferences.putUChar("podIP1", tempIPOctets[1]);
      preferences.putUChar("podIP2", tempIPOctets[2]);
      preferences.putUChar("podIP3", tempIPOctets[3]);

      Serial.printf("Saved Pod IP: %s\n", podIP.toString().c_str());

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

    for (int i = -2; i <= 2; i++)
    {
      int networkIndex = selectedSSIDIndex + i;
      if (networkIndex < 0 || networkIndex >= scannedSSIDCount) continue;

      int yPos = centerY_menu + (i * itemSpacing);
      if (yPos < 50 || yPos > SCREEN_HEIGHT - 30) continue;

      if (i == 0)
      {
        sprite.setFont(&fonts::FreeSansBold12pt7b);
        sprite.setTextColor(accentColor);
        sprite.setTextDatum(middle_center);
        sprite.drawString(scannedSSIDs[networkIndex].c_str(), centerX, yPos);
        sprite.setFont(&fonts::FreeSans9pt7b);
        sprite.drawString(">", centerX - 100, yPos);
        sprite.drawString("<", centerX + 100, yPos);
      }
      else
      {
        sprite.setFont(&fonts::FreeSans9pt7b);
        sprite.setTextColor(dimTextColor);
        sprite.setTextDatum(middle_center);
        sprite.drawString(scannedSSIDs[networkIndex].c_str(), centerX, yPos);
      }
    }

    sprite.setFont(&fonts::Font0);
    sprite.setTextColor(textColor);
    sprite.setTextDatum(middle_center);
    sprite.drawString("Turn to select | Click to connect | Tap to cancel", centerX, SCREEN_HEIGHT - 10);
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
      if (selectedSSIDIndex < 0) selectedSSIDIndex = 0;
      if (selectedSSIDIndex >= scannedSSIDCount) selectedSSIDIndex = scannedSSIDCount - 1;

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
  sprite.drawString(scannedSSIDs[selectedSSIDIndex].c_str(), centerX, 55);

  // Masked password
  sprite.setFont(&fonts::FreeSansBold12pt7b);
  sprite.setTextColor(textColor);
  sprite.setTextDatum(middle_center);
  String maskedPassword = "";
  for (unsigned int i = 0; i < wifiPasswordInput.length(); i++) maskedPassword += "*";
  sprite.drawString(maskedPassword.c_str(), centerX, centerY - 20);

  // Character carousel
  const int charSpacing = 30;
  const int centerY_char = centerY + 40;

  for (int i = -2; i <= 2; i++)
  {
    int charIdx = passwordCharIndex + i;
    int alphaLen = strlen(alphaNumeric);
    charIdx = (charIdx + alphaLen) % alphaLen;
    int yPos = centerY_char + (i * charSpacing);

    if (i == 0)
    {
      sprite.setFont(&fonts::FreeSansBold18pt7b);
      sprite.setTextColor(accentColor);
      char charStr[2] = {alphaNumeric[charIdx], '\0'};
      sprite.drawString(charStr, centerX, yPos);
      sprite.drawRect(centerX - 15, yPos - 18, 30, 36, accentColor);
    }
    else
    {
      sprite.setFont(&fonts::FreeSans12pt7b);
      sprite.setTextColor(textColor);
      char charStr[2] = {alphaNumeric[charIdx], '\0'};
      sprite.drawString(charStr, centerX, yPos);
    }
  }

  sprite.setFont(&fonts::Font0);
  sprite.setTextColor(textColor);
  sprite.setTextDatum(middle_center);
  sprite.drawString("Turn to select char | Click to add | Long press to connect", centerX, SCREEN_HEIGHT - 20);
  sprite.drawString("Tap screen to cancel", centerX, SCREEN_HEIGHT - 10);

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

      int alphaLen = strlen(alphaNumeric);
      passwordCharIndex = (passwordCharIndex + steps + alphaLen) % alphaLen;
      drawPasswordEntry();
    }
  }

  // Click = add character
  if (M5Dial.BtnA.wasPressed())
  {
    recordActivity();
    wifiPasswordInput += alphaNumeric[passwordCharIndex];
    drawPasswordEntry();
  }

  // Long press = submit and connect
  if (M5Dial.BtnA.pressedFor(1000))
  {
    recordActivity();

    WiFi.begin(scannedSSIDs[selectedSSIDIndex].c_str(), wifiPasswordInput.c_str());

    bool nightMode = isNightTime();
    uint16_t bgColor = nightMode ? COLOR_NIGHT_BACKGROUND : COLOR_BACKGROUND;
    uint16_t accentColor = nightMode ? COLOR_NIGHT_SETPOINT : COLOR_SETPOINT;

    sprite.fillSprite(bgColor);
    sprite.setTextColor(accentColor);
    sprite.setTextDatum(middle_center);
    sprite.setFont(&fonts::FreeSans12pt7b);
    sprite.drawString("Connecting...", centerX, centerY);
    sprite.pushSprite(0, 0);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20)
    {
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

      sprite.fillSprite(bgColor);
      sprite.setTextColor(COLOR_SETPOINT);
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
    }

    currentSubMenu = SUBMENU_NONE;
    lastEncoderPosition = M5Dial.Encoder.read();
    drawSettingsMenu();
  }
}

// ==================== Color & Utility ====================

uint16_t getTemperatureColor(float percent)
{
  // 3-segment gradient: Cool Blue -> Teal -> Warm Amber -> Red-Orange
  if (percent < 0.0f) percent = 0.0f;
  if (percent > 1.0f) percent = 1.0f;

  uint8_t r, g, b;

  if (percent < 0.333f)
  {
    float t = percent / 0.333f;
    r = 0;
    g = (uint8_t)(120 + 60 * t);
    b = (uint8_t)(220 - 80 * t);
  }
  else if (percent < 0.666f)
  {
    float t = (percent - 0.333f) / 0.333f;
    r = (uint8_t)(235 * t);
    g = (uint8_t)(180 - 40 * t);
    b = (uint8_t)(140 - 130 * t);
  }
  else
  {
    float t = (percent - 0.666f) / 0.334f;
    r = (uint8_t)(235 + 20 * t);
    g = (uint8_t)(140 - 100 * t);
    b = (uint8_t)(10 - 10 * t);
  }

  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

uint16_t getTemperatureColorNight(float percent)
{
  if (percent < 0.0f) percent = 0.0f;
  if (percent > 1.0f) percent = 1.0f;

  uint8_t r = (uint8_t)(64 + 191 * percent);
  return ((r & 0xF8) << 8); // Red only
}

float mapFloat(float x, float in_min, float in_max, float out_min, float out_max)
{
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
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
  if (nightModeOverride) return true;
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
    updateBrightness();
  }
}

void updateBrightness()
{
  unsigned long timeSinceActivity = millis() - lastActivityTime;
  uint8_t targetBrightness;

  if (timeSinceActivity > DIM_TIMEOUT_MS)
  {
    targetBrightness = BRIGHTNESS_DIM;
    if (!isDimmed)
    {
      isDimmed = true;
    }
  }
  else
  {
    targetBrightness = isNightTime() ? BRIGHTNESS_NIGHT : BRIGHTNESS_DAY;
    isDimmed = false;
  }

  M5Dial.Display.setBrightness(targetBrightness);
}

// ==================== State Helpers ====================

int &getActiveSetpoint()
{
  return rightSideActive ? rightSetpoint : leftSetpoint;
}

int &getInactiveSetpoint()
{
  return rightSideActive ? leftSetpoint : rightSetpoint;
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
  case MENU_DEFAULT_SIDE:   return "Active Side";
  default:                  return "Unknown";
  }
}

// ==================== Pod Sync ====================

void toggleActivePower()
{
  const char *side = rightSideActive ? "right" : "left";

  if (rightSideActive)
  {
    rightPowerOn = !rightPowerOn;
    setPodPower(podIP, side, rightPowerOn, podPort);
  }
  else
  {
    leftPowerOn = !leftPowerOn;
    setPodPower(podIP, side, leftPowerOn, podPort);
  }

  drawTemperatureUI();
}

void syncStatusFromPod()
{
  Serial.println("Syncing status from Pod...");

  // Fetch device status (temperatures, power)
  PodStatus status = fetchPodStatus(podIP, podPort);
  if (status.success)
  {
    if (status.left.valid)
    {
      leftSetpoint = status.left.targetTemperatureF;
      leftCurrentTempF = status.left.currentTemperatureF;
      leftPowerOn = status.left.isPowered;
      Serial.printf("Left synced: target=%d°F actual=%d°F %s\n", leftSetpoint, leftCurrentTempF, leftPowerOn ? "ON" : "OFF");
    }
    if (status.right.valid)
    {
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

    // Sync temperature unit preference from Pod
    bool podUsesF = (settings.temperatureUnit == "F");
    if (podUsesF != useFahrenheit)
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

  if (status.success)
  {
    if (status.left.valid)
    {
      if (leftPowerOn != status.left.isPowered)
      {
        leftPowerOn = status.left.isPowered;
        needsRedraw = true;
      }
      if (leftSetpoint != status.left.targetTemperatureF)
      {
        leftSetpoint = status.left.targetTemperatureF;
        needsRedraw = true;
      }
      if (leftCurrentTempF != status.left.currentTemperatureF)
      {
        leftCurrentTempF = status.left.currentTemperatureF;
        needsRedraw = true;
      }
    }
    if (status.right.valid)
    {
      if (rightPowerOn != status.right.isPowered)
      {
        rightPowerOn = status.right.isPowered;
        needsRedraw = true;
      }
      if (rightSetpoint != status.right.targetTemperatureF)
      {
        rightSetpoint = status.right.targetTemperatureF;
        needsRedraw = true;
      }
      if (rightCurrentTempF != status.right.currentTemperatureF)
      {
        rightCurrentTempF = status.right.currentTemperatureF;
        needsRedraw = true;
      }
    }
  }

  if (needsRedraw) drawTemperatureUI();
}
