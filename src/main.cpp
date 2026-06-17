#include <Arduino.h>
#include <Wire.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>

#include "config.h"
#include "dashboard.h"

namespace {

ESP8266WebServer server(80);
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET_PIN);
Adafruit_ADS1115 ads;

struct Measurements {
  int16_t soilRaw = 0;
  int soilPercent = 0;
  bool soilValid = false;
  int batteryRaw = 0;
  float batteryVoltage = 0.0f;
  int batteryPercent = 0;
};

Measurements measurements;

enum class PumpSource : uint8_t {
  Soil,
  Schedule,
  Manual,
};

bool displayReady = false;
bool adsReady = false;
bool i2cBusAvailable = false;
bool apMode = false;
bool pumpRunning = false;
bool hasCompletedWatering = false;
bool fallbackScheduleTracking = false;
bool fallbackScheduleHasWatered = false;
uint8_t drySampleCount = 0;
uint8_t activeOledAddress = 0;
PumpSource lastPumpSource = PumpSource::Soil;

uint32_t pumpStartedAt = 0;
uint32_t lastPumpStoppedAt = 0;
uint32_t fallbackScheduleAnchorAt = 0;
uint32_t lastSensorAt = 0;
uint32_t lastDisplayAt = 0;
uint32_t lastSerialAt = 0;
uint32_t wateringRunCount = 0;

String systemMessage = "Starting";

template <typename T>
T clampValue(T value, T minimum, T maximum) {
  return value < minimum ? minimum : (value > maximum ? maximum : value);
}

bool intervalElapsed(uint32_t now, uint32_t previous, uint32_t interval) {
  return static_cast<uint32_t>(now - previous) >= interval;
}

bool timeIsSynced() {
  return time(nullptr) > 1700000000;
}

String formattedDateTime(time_t value = 0) {
  if (value == 0) {
    value = time(nullptr);
  }
  if (value <= 1700000000) {
    return String();
  }
  struct tm localTime;
  localtime_r(&value, &localTime);
  char buffer[24];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime);
  return String(buffer);
}

String formattedOledDateTime() {
  if (!timeIsSynced()) {
    return F("Time: syncing...");
  }
  const time_t now = time(nullptr);
  struct tm localTime;
  localtime_r(&now, &localTime);
  char buffer[20];
  strftime(buffer, sizeof(buffer), "%m-%d %H:%M:%S", &localTime);
  return String(buffer);
}

String formattedDuration(uint32_t durationMs) {
  const uint32_t totalSeconds = (durationMs + 999U) / 1000U;
  char buffer[12];
  snprintf(buffer, sizeof(buffer), "%lu:%02lu",
           static_cast<unsigned long>(totalSeconds / 60U),
           static_cast<unsigned long>(totalSeconds % 60U));
  return String(buffer);
}

String formattedHoursMinutes(uint32_t durationMs) {
  const uint32_t totalMinutes = (durationMs + 59999U) / 60000U;
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%luh %02lum",
           static_cast<unsigned long>(totalMinutes / 60U),
           static_cast<unsigned long>(totalMinutes % 60U));
  return String(buffer);
}

uint32_t pumpElapsedMs(uint32_t now) {
  return pumpRunning ? static_cast<uint32_t>(now - pumpStartedAt) : 0;
}

uint32_t pumpRemainingMs(uint32_t now) {
  if (!pumpRunning) {
    return 0;
  }
  const uint32_t elapsed = pumpElapsedMs(now);
  return elapsed >= PUMP_RUN_MS ? 0 : PUMP_RUN_MS - elapsed;
}

uint32_t cooldownRemainingMs(uint32_t now) {
  if (!hasCompletedWatering) {
    return 0;
  }
  const uint32_t elapsed = static_cast<uint32_t>(now - lastPumpStoppedAt);
  return elapsed >= WATERING_COOLDOWN_MS ? 0 : WATERING_COOLDOWN_MS - elapsed;
}

bool i2cAddressResponds(uint8_t address) {
  if (!i2cBusAvailable) {
    return false;
  }
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool checkI2cBusLines() {
  const int sdaState = digitalRead(I2C_SDA_PIN);
  const int sclState = digitalRead(I2C_SCL_PIN);
  Serial.printf("[I2C] SDA(D5/GPIO14)=%s SCL(D6/GPIO12)=%s\n",
                sdaState == HIGH ? "HIGH" : "LOW",
                sclState == HIGH ? "HIGH" : "LOW");
  if (sdaState == LOW || sclState == LOW) {
    Serial.println(
        F("[I2C] Bus stuck LOW; check swapped wires, shorts, and module power"));
    return false;
  }
  return true;
}

void scanI2cBus() {
  if (!i2cBusAvailable) {
    return;
  }
  Serial.println(F("[I2C] Scanning bus"));
  uint8_t count = 0;
  for (uint8_t address = 1; address < 127; ++address) {
    if (i2cAddressResponds(address)) {
      Serial.printf("[I2C] Device found at 0x%02X\n", address);
      ++count;
    }
    yield();
  }
  if (count == 0) {
    Serial.println(F("[I2C] No devices found; check power, ground, SDA, and SCL"));
  }
}

uint8_t detectOledAddress() {
  if (i2cAddressResponds(OLED_PREFERRED_ADDRESS)) {
    return OLED_PREFERRED_ADDRESS;
  }
  if (!OLED_AUTO_DETECT_ADDRESS) {
    return 0;
  }

  constexpr uint8_t commonAddresses[] = {0x3C, 0x3D};
  for (uint8_t address : commonAddresses) {
    if (address != OLED_PREFERRED_ADDRESS && i2cAddressResponds(address)) {
      return address;
    }
  }
  return 0;
}

const __FlashStringHelper* pumpSourceName(PumpSource source) {
  switch (source) {
    case PumpSource::Manual:
      return F("Manual");
    case PumpSource::Schedule:
      return F("Schedule");
    case PumpSource::Soil:
    default:
      return F("Soil");
  }
}

bool scheduleFallbackActive() {
  return AUTOMATIC_WATERING_ENABLED && SENSORLESS_SCHEDULE_ENABLED &&
      !measurements.soilValid;
}

uint32_t scheduleRemainingMs(uint32_t now) {
  if (!scheduleFallbackActive() || !fallbackScheduleTracking) {
    return 0;
  }
  const uint32_t interval = fallbackScheduleHasWatered
      ? SENSORLESS_WATERING_INTERVAL_MS
      : SENSORLESS_FIRST_RUN_DELAY_MS;
  const uint32_t elapsed =
      static_cast<uint32_t>(now - fallbackScheduleAnchorAt);
  return elapsed >= interval ? 0 : interval - elapsed;
}

const __FlashStringHelper* controlModeName() {
  if (measurements.soilValid) {
    return F("Soil sensor");
  }
  return AUTOMATIC_WATERING_ENABLED && SENSORLESS_SCHEDULE_ENABLED
      ? F("Scheduled fallback")
      : F("Manual only");
}

const __FlashStringHelper* controlModeShortName() {
  if (measurements.soilValid) {
    return F("SENSOR");
  }
  return scheduleFallbackActive() ? F("SCHEDULE") : F("MANUAL");
}

void writePump(bool enabled) {
  digitalWrite(PUMP_PIN, enabled == PUMP_ACTIVE_HIGH ? HIGH : LOW);
}

void setRgb(bool red, bool green, bool blue) {
  digitalWrite(LED_RED_PIN, red != LED_COMMON_ANODE ? HIGH : LOW);
  digitalWrite(LED_GREEN_PIN, green != LED_COMMON_ANODE ? HIGH : LOW);
  digitalWrite(LED_BLUE_PIN, blue != LED_COMMON_ANODE ? HIGH : LOW);
}

void updateStatusLed(uint32_t now) {
  if (pumpRunning) {
    setRgb(false, true, true);  // Cyan: pump active.
  } else if (measurements.batteryVoltage < BATTERY_PUMP_CUTOFF_V) {
    setRgb(true, false, false);  // Red: low battery.
  } else if (scheduleFallbackActive()) {
    setRgb(false, false, true);  // Blue: scheduled fallback.
  } else if (!adsReady || !measurements.soilValid) {
    setRgb(true, false, true);  // Magenta: sensor error, manual only.
  } else if (WiFi.status() != WL_CONNECTED && !apMode) {
    const bool blink = ((now / 500U) % 2U) == 0U;
    setRgb(false, false, blink);  // Blinking blue: connecting.
  } else if (measurements.soilPercent <= SOIL_START_WATERING_PERCENT) {
    setRgb(true, true, false);  // Yellow: dry or waiting for cooldown.
  } else {
    setRgb(false, true, false);  // Green: normal.
  }
}

int averageA0() {
  uint32_t total = 0;
  constexpr uint8_t sampleCount = 16;
  for (uint8_t i = 0; i < sampleCount; ++i) {
    total += analogRead(A0);
    delay(1);
  }
  return static_cast<int>(total / sampleCount);
}

void readSensors() {
  measurements.batteryRaw = averageA0();
  measurements.batteryVoltage =
      (measurements.batteryRaw / 1023.0f) *
      BATTERY_ADC_FULL_SCALE_V * BATTERY_DIVIDER_RATIO;
  const float batteryRange = BATTERY_FULL_V - BATTERY_EMPTY_V;
  const float batteryRatio = batteryRange > 0.0f
      ? (measurements.batteryVoltage - BATTERY_EMPTY_V) / batteryRange
      : 0.0f;
  measurements.batteryPercent =
      clampValue(static_cast<int>(lroundf(batteryRatio * 100.0f)), 0, 100);

  if (!adsReady) {
    measurements.soilValid = false;
    drySampleCount = 0;
    return;
  }

  measurements.soilRaw = ads.readADC_SingleEnded(SOIL_ADS_CHANNEL);
  const int32_t calibrationSpan =
      static_cast<int32_t>(SOIL_WET_RAW) - SOIL_DRY_RAW;
  measurements.soilValid =
      calibrationSpan != 0 && measurements.soilRaw >= 0;

  if (!measurements.soilValid) {
    drySampleCount = 0;
    return;
  }

  const float soilRatio =
      static_cast<float>(measurements.soilRaw - SOIL_DRY_RAW) /
      static_cast<float>(calibrationSpan);
  measurements.soilPercent =
      clampValue(static_cast<int>(lroundf(soilRatio * 100.0f)), 0, 100);

  if (measurements.soilPercent <= SOIL_START_WATERING_PERCENT) {
    if (drySampleCount < SOIL_DRY_CONFIRMATION_SAMPLES) {
      ++drySampleCount;
    }
  } else {
    drySampleCount = 0;
  }
}

bool startPump(PumpSource source) {
  if (pumpRunning) {
    systemMessage = "Pump is already running";
    return false;
  }
  if (measurements.batteryVoltage < BATTERY_PUMP_CUTOFF_V) {
    systemMessage = "Pump blocked: battery voltage is too low";
    return false;
  }

  lastPumpSource = source;
  pumpStartedAt = millis();
  ++wateringRunCount;
  pumpRunning = true;
  writePump(true);
  switch (source) {
    case PumpSource::Manual:
      systemMessage = "Manual watering in progress";
      break;
    case PumpSource::Schedule:
      systemMessage = "Scheduled watering in progress";
      break;
    case PumpSource::Soil:
      systemMessage = "Soil-triggered watering in progress";
      break;
  }
  Serial.printf("[PUMP] Started (%s), duration=%lu ms\n",
                String(pumpSourceName(source)).c_str(),
                static_cast<unsigned long>(PUMP_RUN_MS));
  return true;
}

void stopPump(const __FlashStringHelper* reason) {
  if (!pumpRunning) {
    return;
  }
  writePump(false);
  pumpRunning = false;
  lastPumpStoppedAt = millis();
  hasCompletedWatering = true;
  drySampleCount = 0;
  if (scheduleFallbackActive()) {
    fallbackScheduleTracking = true;
    fallbackScheduleHasWatered = true;
    fallbackScheduleAnchorAt = lastPumpStoppedAt;
  }
  systemMessage = String(reason);
  Serial.printf("[PUMP] Stopped: %s\n", systemMessage.c_str());
}

void updatePumpControl(uint32_t now, bool evaluateAutomaticStart) {
  if (pumpRunning) {
    if (pumpElapsedMs(now) >= PUMP_RUN_MS) {
      stopPump(F("Watering complete; soil is soaking"));
    } else if (measurements.batteryVoltage < BATTERY_PUMP_CUTOFF_V) {
      stopPump(F("Pump stopped: battery voltage became too low"));
    }
    return;
  }

  if (!AUTOMATIC_WATERING_ENABLED) {
    return;
  }

  if (!measurements.soilValid) {
    if (!SENSORLESS_SCHEDULE_ENABLED) {
      fallbackScheduleTracking = false;
      return;
    }
    if (!fallbackScheduleTracking) {
      fallbackScheduleTracking = true;
      fallbackScheduleHasWatered = false;
      fallbackScheduleAnchorAt = now;
      systemMessage = "Soil unavailable; scheduled fallback active";
      Serial.printf("[SCHEDULE] Fallback enabled, first run in %lu ms\n",
                    static_cast<unsigned long>(
                        SENSORLESS_FIRST_RUN_DELAY_MS));
      return;
    }
    if (scheduleRemainingMs(now) > 0 || cooldownRemainingMs(now) > 0) {
      return;
    }
    startPump(PumpSource::Schedule);
    return;
  }

  fallbackScheduleTracking = false;
  fallbackScheduleHasWatered = false;
  if (!evaluateAutomaticStart ||
      drySampleCount < SOIL_DRY_CONFIRMATION_SAMPLES) {
    return;
  }
  if (cooldownRemainingMs(now) > 0) {
    systemMessage = "Soil is dry; waiting for soak cooldown";
    return;
  }
  startPump(PumpSource::Soil);
}

String wifiDescription() {
  if (apMode) {
    return "Setup access point";
  }
  return WiFi.status() == WL_CONNECTED ? "Wi-Fi connected"
                                       : "Wi-Fi disconnected";
}

String currentIp() {
  return apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

String statusJson() {
  const uint32_t now = millis();
  const uint32_t nextScheduledMs = scheduleRemainingMs(now);
  String nextScheduledDateTime;
  if (timeIsSynced() && scheduleFallbackActive()) {
    nextScheduledDateTime =
        formattedDateTime(time(nullptr) + ((nextScheduledMs + 999U) / 1000U));
  }
  String json;
  json.reserve(720);
  json += F("{\"wifi\":\"");
  json += wifiDescription();
  json += F("\",\"ip\":\"");
  json += currentIp();
  json += F("\",\"soilValid\":");
  json += measurements.soilValid ? F("true") : F("false");
  json += F(",\"soilRaw\":");
  json += measurements.soilRaw;
  json += F(",\"soilPercent\":");
  json += measurements.soilPercent;
  json += F(",\"batteryRaw\":");
  json += measurements.batteryRaw;
  json += F(",\"batteryVoltage\":");
  json += String(measurements.batteryVoltage, 3);
  json += F(",\"batteryPercent\":");
  json += measurements.batteryPercent;
  json += F(",\"pumpRunning\":");
  json += pumpRunning ? F("true") : F("false");
  json += F(",\"pumpSource\":\"");
  json += wateringRunCount == 0 ? F("None") : pumpSourceName(lastPumpSource);
  json += F("\",\"pumpRemainingMs\":");
  json += pumpRemainingMs(now);
  json += F(",\"pumpElapsedMs\":");
  json += pumpElapsedMs(now);
  json += F(",\"pumpDurationMs\":");
  json += PUMP_RUN_MS;
  json += F(",\"wateringRunCount\":");
  json += wateringRunCount;
  json += F(",\"cooldownRemainingMs\":");
  json += cooldownRemainingMs(now);
  json += F(",\"automaticEnabled\":");
  json += AUTOMATIC_WATERING_ENABLED ? F("true") : F("false");
  json += F(",\"controlMode\":\"");
  json += controlModeName();
  json += F("\",\"scheduleFallbackActive\":");
  json += scheduleFallbackActive() ? F("true") : F("false");
  json += F(",\"nextScheduledWateringMs\":");
  json += nextScheduledMs;
  json += F(",\"nextScheduledDateTime\":\"");
  json += nextScheduledDateTime;
  json += F("\",\"timeSynced\":");
  json += timeIsSynced() ? F("true") : F("false");
  json += F(",\"currentDateTime\":\"");
  json += formattedDateTime();
  json += F("\"");
  json += F(",\"oledReady\":");
  json += displayReady ? F("true") : F("false");
  json += F(",\"oledAddress\":");
  json += activeOledAddress;
  json += F(",\"uptimeMs\":");
  json += now;
  json += F(",\"message\":\"");
  json += systemMessage;
  json += F("\"}");
  return json;
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", DASHBOARD_HTML);
  });
  server.on("/api/status", HTTP_GET, []() {
    server.send(200, "application/json", statusJson());
  });
  server.on("/pump", HTTP_POST, []() {
    const bool started = startPump(PumpSource::Manual);
    String response = F("{\"started\":");
    response += started ? F("true") : F("false");
    response += F(",\"message\":\"");
    response += systemMessage;
    response += F("\"}");
    server.send(started ? 202 : 409, "application/json", response);
  });
  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });
  server.begin();
  Serial.println(F("[WEB] HTTP server started"));
}

void showStartupMessage(const __FlashStringHelper* line1,
                        const String& line2 = String()) {
  if (!displayReady) {
    return;
  }
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println(line1);
  display.println(line2);
  display.display();
}

void connectNetwork() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("[WIFI] Connecting to %s", WIFI_SSID);
  showStartupMessage(F("Connecting Wi-Fi"), WIFI_SSID);
  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         !intervalElapsed(millis(), startedAt, 20000)) {
    Serial.print('.');
    updateStatusLed(millis());
    delay(250);
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    systemMessage = "Controller ready";
    Serial.printf("[WIFI] Connected, IP=%s\n",
                  WiFi.localIP().toString().c_str());
    showStartupMessage(F("Wi-Fi connected"), WiFi.localIP().toString());
    if (MDNS.begin(HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("[MDNS] http://%s.local/\n", HOSTNAME);
    }
    configTime(TIMEZONE, NTP_SERVER_1, NTP_SERVER_2);
    Serial.println(F("[TIME] NTP synchronization started"));
    return;
  }

  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  apMode = WiFi.softAP("Plant-Watering-Setup", AP_PASSWORD);
  systemMessage = apMode ? "Wi-Fi failed; setup access point active"
                         : "Wi-Fi and access point failed";
  Serial.printf("[WIFI] AP %s, IP=%s\n",
                apMode ? "started" : "failed",
                WiFi.softAPIP().toString().c_str());
  showStartupMessage(F("Setup AP active"), WiFi.softAPIP().toString());
}

void updateDisplay(uint32_t now) {
  if (!displayReady) {
    return;
  }

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println(formattedOledDateTime());
  display.print(F("IP: "));
  display.println(currentIp());

  display.print(F("Mode: "));
  display.println(controlModeShortName());

  display.print(F("Soil:"));
  if (measurements.soilValid) {
    display.print(measurements.soilPercent);
    display.print(F("%"));
  } else {
    display.print(F("--"));
  }
  display.print(F(" Bat:"));
  display.print(measurements.batteryPercent);
  display.println(F("%"));

  if (pumpRunning) {
    display.print(F("Water #"));
    display.print(wateringRunCount);
    display.print(' ');
    display.println(pumpSourceName(lastPumpSource));
    display.print(F("Run "));
    display.print(formattedDuration(pumpElapsedMs(now)));
    display.print('/');
    display.println(formattedDuration(PUMP_RUN_MS));
    display.print(F("Left "));
    display.println(formattedDuration(pumpRemainingMs(now)));
  } else {
    display.print(F("Pump OFF Runs:"));
    display.println(wateringRunCount);
    const uint32_t scheduled = scheduleRemainingMs(now);
    const uint32_t cooldown = cooldownRemainingMs(now);
    if (scheduleFallbackActive()) {
      display.print(F("Next "));
      display.println(formattedHoursMinutes(scheduled));
    } else if (cooldown > 0) {
      display.print(F("Soak "));
      display.println(formattedHoursMinutes(cooldown));
    } else {
      display.println(F("Ready"));
    }
  }
  display.display();
}

void printDiagnostics(uint32_t now) {
  Serial.printf(
      "[STATUS] uptime=%lus wifi=\"%s\" ip=%s soil=%d%% raw=%d valid=%s "
      "battery=%.3fV %d%% raw=%d pump=%s source=%s remaining=%lums "
      "cooldown=%lums mode=\"%s\" schedule_remaining=%lums oled=%s@0x%02X "
      "time=\"%s\" run_count=%lu message=\"%s\"\n",
      static_cast<unsigned long>(now / 1000U),
      wifiDescription().c_str(),
      currentIp().c_str(),
      measurements.soilPercent,
      measurements.soilRaw,
      measurements.soilValid ? "yes" : "no",
      measurements.batteryVoltage,
      measurements.batteryPercent,
      measurements.batteryRaw,
      pumpRunning ? "on" : "off",
      String(pumpSourceName(lastPumpSource)).c_str(),
      static_cast<unsigned long>(pumpRemainingMs(now)),
      static_cast<unsigned long>(cooldownRemainingMs(now)),
      String(controlModeName()).c_str(),
      static_cast<unsigned long>(scheduleRemainingMs(now)),
      displayReady ? "ready" : "missing",
      activeOledAddress,
      formattedDateTime().c_str(),
      static_cast<unsigned long>(wateringRunCount),
      systemMessage.c_str());
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("[BOOT] ESP8266 plant watering controller"));

  pinMode(PUMP_PIN, OUTPUT);
  writePump(false);
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(LED_BLUE_PIN, OUTPUT);
  setRgb(false, false, false);

  if (OLED_RESET_PIN >= 0) {
    pinMode(OLED_RESET_PIN, OUTPUT);
    digitalWrite(OLED_RESET_PIN, LOW);
    delay(10);
    digitalWrite(OLED_RESET_PIN, HIGH);
    delay(20);
  }

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);
  Wire.setClockStretchLimit(I2C_CLOCK_STRETCH_LIMIT_US);
  delay(50);
  i2cBusAvailable = checkI2cBusLines();
  scanI2cBus();
  activeOledAddress = detectOledAddress();
  displayReady = activeOledAddress != 0 &&
      display.begin(SSD1306_SWITCHCAPVCC, activeOledAddress, true, false);
  if (displayReady) {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setTextWrap(false);
    display.setRotation(OLED_ROTATION);
    showStartupMessage(F("Plant controller"), F("Starting..."));
    Serial.printf("[OLED] Ready at 0x%02X, %ux%u, rotation=%u\n",
                  activeOledAddress, OLED_WIDTH, OLED_HEIGHT, OLED_ROTATION);
  } else {
    Serial.println(
        F("[OLED] SSD1306 not detected at 0x3C/0x3D; check wiring or controller type"));
  }

  adsReady = i2cBusAvailable && ads.begin(ADS1115_ADDRESS);
  if (adsReady) {
    ads.setGain(GAIN_ONE);
    Serial.println(F("[SOIL] ADS1115 ready"));
  } else {
    Serial.println(F("[SOIL] ADS1115 not found"));
  }

  readSensors();
  connectNetwork();
  setupWebServer();
  delay(500);
}

void loop() {
  const uint32_t now = millis();
  bool sensorSampleTaken = false;
  server.handleClient();
  if (!apMode && WiFi.status() == WL_CONNECTED) {
    MDNS.update();
  }

  if (intervalElapsed(now, lastSensorAt, SENSOR_INTERVAL_MS)) {
    lastSensorAt = now;
    readSensors();
    sensorSampleTaken = true;
  }

  updatePumpControl(now, sensorSampleTaken);
  updateStatusLed(now);

  if (intervalElapsed(now, lastDisplayAt, DISPLAY_INTERVAL_MS)) {
    lastDisplayAt = now;
    updateDisplay(now);
  }

  if (intervalElapsed(now, lastSerialAt, SERIAL_INTERVAL_MS)) {
    lastSerialAt = now;
    printDiagnostics(now);
  }

  yield();
}
