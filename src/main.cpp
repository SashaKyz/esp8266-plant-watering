#include <Arduino.h>
#include <Wire.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiClient.h>
#include <EEPROM.h>
#include <Updater.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>

#include "config.h"
#include "dashboard.h"

namespace {

bool serverSyncConfigured();

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
bool displayOn = false;
bool adsReady = false;
bool i2cBusAvailable = false;
bool apMode = false;
bool pumpRunning = false;
bool hasCompletedWatering = false;
bool fallbackScheduleTracking = false;
bool fallbackScheduleHasWatered = false;
bool otaInProgress = false;
bool otaUploadRejected = false;
bool otaRebootPending = false;
bool wokeFromDeepSleep = false;
bool wifiSleeping = false;
bool wifiWasConnected = false;
bool serverSyncDue = false;
bool serverSyncSucceededThisWifiSession = false;
uint8_t drySampleCount = 0;
uint8_t activeOledAddress = 0;
PumpSource lastPumpSource = PumpSource::Soil;

uint32_t pumpStartedAt = 0;
uint32_t lastPumpStoppedAt = 0;
uint32_t fallbackScheduleAnchorAt = 0;
uint32_t lastSensorAt = 0;
uint32_t lastDisplayAt = 0;
uint32_t lastOledActivityAt = 0;
uint32_t lastSerialAt = 0;
uint32_t wateringRunCount = 0;
uint32_t otaRebootAt = 0;
uint32_t pumpRunMs = PUMP_RUN_DEFAULT_MS;
uint32_t pauseStartedAt = 0;
uint32_t pauseDurationMs = 0;
uint32_t lastPersistentSaveAt = 0;
uint32_t wifiModeChangedAt = 0;
uint32_t lastServerSyncAt = 0;
uint32_t lastSuccessfulServerSyncAt = 0;
uint32_t nextServerSyncRetryAt = 0;
uint32_t nextWifiConnectRetryAt = 0;
uint32_t serverConfigVersion = 0;
uint32_t activeWateringCooldownMs = WATERING_COOLDOWN_MS;
uint32_t activeSensorIntervalMs = SENSOR_INTERVAL_MS;
uint32_t activeWifiOnWindowMs = WIFI_ON_WINDOW_MS;
uint32_t activeWifiOffWindowMs = WIFI_OFF_WINDOW_MS;
uint32_t activeOledIdleTimeoutMs = OLED_IDLE_TIMEOUT_MS;
time_t wateringPausedUntil = 0;
time_t lastPumpStoppedAtEpoch = 0;
int lastServerTelemetryStatus = 0;
int lastServerConfigStatus = 0;
int lastServerOtaStatus = 0;
uint8_t activeSoilStartWateringPercent = SOIL_START_WATERING_PERCENT;
bool activeAutomaticWateringEnabled = AUTOMATIC_WATERING_ENABLED;
bool serverOtaUpdateAvailable = false;
uint32_t serverOtaSize = 0;
String serverOtaVersion;
String serverOtaUrl;
String serverOtaSha256;
String lastServerSyncMessage = "Server sync disabled";

constexpr uint32_t RTC_STATE_MAGIC = 0x50574D31;
struct RtcState {
  uint32_t magic;
  uint32_t pauseUntilEpoch;
};

constexpr uint32_t LEGACY_SETTINGS_MAGIC = 0x50575331;
struct LegacyPersistentSettings {
  uint32_t magic;
  uint32_t pumpRunMs;
  uint32_t checksum;
};

constexpr uint32_t PERSISTENT_STATE_MAGIC = 0x50575332;
constexpr uint32_t PERSISTENT_STATE_V3_MAGIC = 0x50575333;
constexpr uint32_t PERSISTENT_STATE_V4_MAGIC = 0x50575334;
constexpr size_t EEPROM_SIZE = 64;
struct PersistentState {
  uint32_t magic;
  uint32_t pumpRunMs;
  uint32_t wateringPausedUntilEpoch;
  uint32_t lastPumpStoppedEpoch;
  uint32_t wateringRunCount;
  uint32_t checksum;
};

struct PersistentStateV3 {
  uint32_t magic;
  uint32_t pumpRunMs;
  uint32_t wateringPausedUntilEpoch;
  uint32_t lastPumpStoppedEpoch;
  uint32_t wateringRunCount;
  uint32_t serverConfigVersion;
  uint32_t soilStartWateringPercent;
  uint32_t wateringCooldownMs;
  uint32_t sensorIntervalMs;
  uint32_t wifiOnWindowMs;
  uint32_t wifiOffWindowMs;
  uint32_t automaticWateringEnabled;
  uint32_t checksum;
};

struct PersistentStateV4 {
  uint32_t magic;
  uint32_t pumpRunMs;
  uint32_t wateringPausedUntilEpoch;
  uint32_t lastPumpStoppedEpoch;
  uint32_t wateringRunCount;
  uint32_t serverConfigVersion;
  uint32_t soilStartWateringPercent;
  uint32_t wateringCooldownMs;
  uint32_t sensorIntervalMs;
  uint32_t wifiOnWindowMs;
  uint32_t wifiOffWindowMs;
  uint32_t oledIdleTimeoutMs;
  uint32_t automaticWateringEnabled;
  uint32_t checksum;
};

String systemMessage = "Starting";

template <typename T>
T clampValue(T value, T minimum, T maximum) {
  return value < minimum ? minimum : (value > maximum ? maximum : value);
}

bool intervalElapsed(uint32_t now, uint32_t previous, uint32_t interval) {
  return static_cast<uint32_t>(now - previous) >= interval;
}

bool deadlineReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

uint32_t randomRetryDelayMs() {
  const uint32_t minimum = SERVER_SYNC_RETRY_MIN_MS;
  const uint32_t maximum = SERVER_SYNC_RETRY_MAX_MS < minimum
      ? minimum
      : SERVER_SYNC_RETRY_MAX_MS;
  return static_cast<uint32_t>(random(minimum, maximum + 1UL));
}

void wakeDisplay(uint32_t now) {
  if (!displayReady) {
    return;
  }
  lastOledActivityAt = now;
  if (!displayOn) {
    display.ssd1306_command(SSD1306_DISPLAYON);
    displayOn = true;
  }
}

void sleepDisplay() {
  if (!displayReady || !displayOn) {
    return;
  }
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  displayOn = false;
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

String formattedUtcDateTime(time_t value = 0) {
  if (value == 0) {
    value = time(nullptr);
  }
  if (value <= 1700000000) {
    return String();
  }
  struct tm utcTime;
  gmtime_r(&value, &utcTime);
  char buffer[24];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utcTime);
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
  strftime(buffer, sizeof(buffer), "%m-%d %H:%M", &localTime);
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

String formattedServerSyncAge(uint32_t now) {
  if (!serverSyncConfigured()) {
    return F("off");
  }
  if (lastSuccessfulServerSyncAt == 0) {
    return F("never");
  }
  return formattedHoursMinutes(
      static_cast<uint32_t>(now - lastSuccessfulServerSyncAt));
}

uint32_t pumpElapsedMs(uint32_t now) {
  return pumpRunning ? static_cast<uint32_t>(now - pumpStartedAt) : 0;
}

uint32_t pumpRemainingMs(uint32_t now) {
  if (!pumpRunning) {
    return 0;
  }
  const uint32_t elapsed = pumpElapsedMs(now);
  return elapsed >= pumpRunMs ? 0 : pumpRunMs - elapsed;
}

uint32_t cooldownRemainingMs(uint32_t now) {
  if (!hasCompletedWatering) {
    return 0;
  }
  if (lastPumpStoppedAtEpoch > 0 && timeIsSynced()) {
    const time_t current = time(nullptr);
    if (current <= lastPumpStoppedAtEpoch) {
      return activeWateringCooldownMs;
    }
    const uint64_t elapsedMs =
        static_cast<uint64_t>(current - lastPumpStoppedAtEpoch) * 1000ULL;
    return elapsedMs >= activeWateringCooldownMs
        ? 0
        : activeWateringCooldownMs - static_cast<uint32_t>(elapsedMs);
  }
  const uint32_t elapsed = static_cast<uint32_t>(now - lastPumpStoppedAt);
  return elapsed >= activeWateringCooldownMs
      ? 0
      : activeWateringCooldownMs - elapsed;
}

uint32_t startupPumpHoldRemainingMs(uint32_t now) {
  if (wokeFromDeepSleep) {
    return 0;
  }
  return now >= STARTUP_PUMP_HOLD_MS ? 0 : STARTUP_PUMP_HOLD_MS - now;
}

void saveRtcState() {
  RtcState state = {
      RTC_STATE_MAGIC, static_cast<uint32_t>(wateringPausedUntil)};
  ESP.rtcUserMemoryWrite(0, reinterpret_cast<uint32_t*>(&state),
                         sizeof(state));
}

void loadRtcState() {
  RtcState state = {};
  if (ESP.rtcUserMemoryRead(0, reinterpret_cast<uint32_t*>(&state),
                            sizeof(state)) &&
      state.magic == RTC_STATE_MAGIC) {
    wateringPausedUntil = static_cast<time_t>(state.pauseUntilEpoch);
  }
}

uint32_t legacySettingsChecksum(const LegacyPersistentSettings& settings) {
  return settings.magic ^ settings.pumpRunMs ^ 0xA5A55A5A;
}

uint32_t persistentStateChecksum(const PersistentState& state) {
  return state.magic ^ state.pumpRunMs ^ state.wateringPausedUntilEpoch ^
      state.lastPumpStoppedEpoch ^ state.wateringRunCount ^ 0x5A5AA5A5;
}

uint32_t persistentStateV3Checksum(const PersistentStateV3& state) {
  return state.magic ^ state.pumpRunMs ^ state.wateringPausedUntilEpoch ^
      state.lastPumpStoppedEpoch ^ state.wateringRunCount ^
      state.serverConfigVersion ^ state.soilStartWateringPercent ^
      state.wateringCooldownMs ^ state.sensorIntervalMs ^
      state.wifiOnWindowMs ^ state.wifiOffWindowMs ^
      state.automaticWateringEnabled ^ 0x33A55A33;
}

uint32_t persistentStateV4Checksum(const PersistentStateV4& state) {
  return state.magic ^ state.pumpRunMs ^ state.wateringPausedUntilEpoch ^
      state.lastPumpStoppedEpoch ^ state.wateringRunCount ^
      state.serverConfigVersion ^ state.soilStartWateringPercent ^
      state.wateringCooldownMs ^ state.sensorIntervalMs ^
      state.wifiOnWindowMs ^ state.wifiOffWindowMs ^
      state.oledIdleTimeoutMs ^ state.automaticWateringEnabled ^ 0x34A55A34;
}

void savePersistentState() {
  PersistentStateV4 state = {
      PERSISTENT_STATE_V4_MAGIC,
      pumpRunMs,
      static_cast<uint32_t>(wateringPausedUntil),
      static_cast<uint32_t>(lastPumpStoppedAtEpoch),
      wateringRunCount,
      serverConfigVersion,
      activeSoilStartWateringPercent,
      activeWateringCooldownMs,
      activeSensorIntervalMs,
      activeWifiOnWindowMs,
      activeWifiOffWindowMs,
      activeOledIdleTimeoutMs,
      activeAutomaticWateringEnabled ? 1U : 0U,
      0};
  state.checksum = persistentStateV4Checksum(state);
  EEPROM.put(0, state);
  EEPROM.commit();
  lastPersistentSaveAt = millis();
}

void loadPersistentState() {
  EEPROM.begin(EEPROM_SIZE);
  PersistentStateV4 stateV4 = {};
  EEPROM.get(0, stateV4);
  if (stateV4.magic == PERSISTENT_STATE_V4_MAGIC &&
      stateV4.checksum == persistentStateV4Checksum(stateV4)) {
    if (stateV4.pumpRunMs >= PUMP_RUN_MIN_MS &&
        stateV4.pumpRunMs <= PUMP_RUN_MAX_MS) {
      pumpRunMs = stateV4.pumpRunMs;
    }
    wateringPausedUntil = static_cast<time_t>(stateV4.wateringPausedUntilEpoch);
    lastPumpStoppedAtEpoch = static_cast<time_t>(stateV4.lastPumpStoppedEpoch);
    hasCompletedWatering = lastPumpStoppedAtEpoch > 0;
    wateringRunCount = stateV4.wateringRunCount;
    serverConfigVersion = stateV4.serverConfigVersion;
    activeSoilStartWateringPercent = static_cast<uint8_t>(
        clampValue<uint32_t>(stateV4.soilStartWateringPercent, 0, 100));
    activeWateringCooldownMs = stateV4.wateringCooldownMs;
    activeSensorIntervalMs = stateV4.sensorIntervalMs < 1000UL
        ? SENSOR_INTERVAL_MS
        : stateV4.sensorIntervalMs;
    activeWifiOnWindowMs = stateV4.wifiOnWindowMs < 1000UL
        ? WIFI_ON_WINDOW_MS
        : stateV4.wifiOnWindowMs;
    activeWifiOffWindowMs = stateV4.wifiOffWindowMs < 1000UL
        ? WIFI_OFF_WINDOW_MS
        : stateV4.wifiOffWindowMs;
    activeOledIdleTimeoutMs = stateV4.oledIdleTimeoutMs;
    activeAutomaticWateringEnabled = stateV4.automaticWateringEnabled != 0;
    return;
  }

  PersistentStateV3 stateV3 = {};
  EEPROM.get(0, stateV3);
  if (stateV3.magic == PERSISTENT_STATE_V3_MAGIC &&
      stateV3.checksum == persistentStateV3Checksum(stateV3)) {
    if (stateV3.pumpRunMs >= PUMP_RUN_MIN_MS &&
        stateV3.pumpRunMs <= PUMP_RUN_MAX_MS) {
      pumpRunMs = stateV3.pumpRunMs;
    }
    wateringPausedUntil = static_cast<time_t>(stateV3.wateringPausedUntilEpoch);
    lastPumpStoppedAtEpoch = static_cast<time_t>(stateV3.lastPumpStoppedEpoch);
    hasCompletedWatering = lastPumpStoppedAtEpoch > 0;
    wateringRunCount = stateV3.wateringRunCount;
    serverConfigVersion = stateV3.serverConfigVersion;
    activeSoilStartWateringPercent = static_cast<uint8_t>(
        clampValue<uint32_t>(stateV3.soilStartWateringPercent, 0, 100));
    activeWateringCooldownMs = stateV3.wateringCooldownMs;
    activeSensorIntervalMs = stateV3.sensorIntervalMs < 1000UL
        ? SENSOR_INTERVAL_MS
        : stateV3.sensorIntervalMs;
    activeWifiOnWindowMs = stateV3.wifiOnWindowMs < 1000UL
        ? WIFI_ON_WINDOW_MS
        : stateV3.wifiOnWindowMs;
    activeWifiOffWindowMs = stateV3.wifiOffWindowMs < 1000UL
        ? WIFI_OFF_WINDOW_MS
        : stateV3.wifiOffWindowMs;
    activeOledIdleTimeoutMs = OLED_IDLE_TIMEOUT_MS;
    activeAutomaticWateringEnabled = stateV3.automaticWateringEnabled != 0;
    return;
  }

  PersistentState state = {};
  EEPROM.get(0, state);
  if (state.magic == PERSISTENT_STATE_MAGIC &&
      state.checksum == persistentStateChecksum(state)) {
    if (state.pumpRunMs >= PUMP_RUN_MIN_MS &&
        state.pumpRunMs <= PUMP_RUN_MAX_MS) {
      pumpRunMs = state.pumpRunMs;
    }
    wateringPausedUntil = static_cast<time_t>(state.wateringPausedUntilEpoch);
    lastPumpStoppedAtEpoch = static_cast<time_t>(state.lastPumpStoppedEpoch);
    hasCompletedWatering = lastPumpStoppedAtEpoch > 0;
    wateringRunCount = state.wateringRunCount;
    return;
  }

  LegacyPersistentSettings legacy = {};
  EEPROM.get(0, legacy);
  if (legacy.magic == LEGACY_SETTINGS_MAGIC &&
      legacy.checksum == legacySettingsChecksum(legacy) &&
      legacy.pumpRunMs >= PUMP_RUN_MIN_MS &&
      legacy.pumpRunMs <= PUMP_RUN_MAX_MS) {
    pumpRunMs = legacy.pumpRunMs;
    savePersistentState();
  }
}

uint32_t wateringPauseRemainingMs(uint32_t now) {
  if (wateringPausedUntil > 0) {
    if (!timeIsSynced()) {
      return UINT32_MAX;
    }
    const time_t current = time(nullptr);
    if (current >= wateringPausedUntil) {
      wateringPausedUntil = 0;
      saveRtcState();
      savePersistentState();
      return 0;
    }
    const uint64_t remaining =
        static_cast<uint64_t>(wateringPausedUntil - current) * 1000ULL;
    return remaining > UINT32_MAX ? UINT32_MAX
                                  : static_cast<uint32_t>(remaining);
  }
  if (pauseDurationMs == 0) {
    return 0;
  }
  const uint32_t elapsed = static_cast<uint32_t>(now - pauseStartedAt);
  if (elapsed >= pauseDurationMs) {
    pauseDurationMs = 0;
    return 0;
  }
  return pauseDurationMs - elapsed;
}

bool wateringPaused(uint32_t now) {
  return wateringPauseRemainingMs(now) > 0;
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
  return activeAutomaticWateringEnabled && SENSORLESS_SCHEDULE_ENABLED &&
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
  return activeAutomaticWateringEnabled && SENSORLESS_SCHEDULE_ENABLED
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
  if (!DEEP_SLEEP_ENABLED) {
    digitalWrite(LED_BLUE_PIN, blue != LED_COMMON_ANODE ? HIGH : LOW);
  }
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
  } else if (measurements.soilPercent <= activeSoilStartWateringPercent) {
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

int liIon18650Percent(float voltage) {
  struct VoltagePoint {
    float voltage;
    uint8_t percent;
  };
  constexpr VoltagePoint curve[] = {
      {3.20f, 0},  {3.40f, 5},  {3.50f, 10}, {3.60f, 20},
      {3.65f, 35}, {3.70f, 50}, {3.75f, 60}, {3.80f, 70},
      {3.90f, 80}, {4.00f, 90}, {4.10f, 96}, {4.20f, 100},
  };

  if (voltage <= curve[0].voltage) {
    return curve[0].percent;
  }
  constexpr size_t pointCount = sizeof(curve) / sizeof(curve[0]);
  for (size_t i = 1; i < pointCount; ++i) {
    if (voltage <= curve[i].voltage) {
      const float position =
          (voltage - curve[i - 1].voltage) /
          (curve[i].voltage - curve[i - 1].voltage);
      return static_cast<int>(lroundf(
          curve[i - 1].percent +
          position * (curve[i].percent - curve[i - 1].percent)));
    }
  }
  return 100;
}

void readSensors() {
  measurements.batteryRaw = averageA0();
  measurements.batteryVoltage =
      (measurements.batteryRaw / 1023.0f) *
      BATTERY_ADC_FULL_SCALE_V * BATTERY_DIVIDER_RATIO;
  measurements.batteryPercent = liIon18650Percent(measurements.batteryVoltage);

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

  if (measurements.soilPercent <= activeSoilStartWateringPercent) {
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
  const uint32_t startupHoldRemaining = startupPumpHoldRemainingMs(millis());
  if (startupHoldRemaining > 0) {
    systemMessage = "Pump held after startup for " +
        formattedDuration(startupHoldRemaining);
    return false;
  }
  if (wateringPaused(millis())) {
    systemMessage = "Watering is paused";
    return false;
  }
  if (measurements.batteryVoltage < BATTERY_PUMP_CUTOFF_V) {
    systemMessage = "Pump blocked: battery voltage is too low";
    return false;
  }

  lastPumpSource = source;
  pumpStartedAt = millis();
  wakeDisplay(pumpStartedAt);
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
                static_cast<unsigned long>(pumpRunMs));
  return true;
}

void stopPump(const __FlashStringHelper* reason) {
  if (!pumpRunning) {
    return;
  }
  writePump(false);
  pumpRunning = false;
  lastPumpStoppedAt = millis();
  lastPumpStoppedAtEpoch = timeIsSynced() ? time(nullptr) : 0;
  wakeDisplay(lastPumpStoppedAt);
  hasCompletedWatering = true;
  drySampleCount = 0;
  if (scheduleFallbackActive()) {
    fallbackScheduleTracking = true;
    fallbackScheduleHasWatered = true;
    fallbackScheduleAnchorAt = lastPumpStoppedAt;
  }
  systemMessage = String(reason);
  savePersistentState();
  Serial.printf("[PUMP] Stopped: %s\n", systemMessage.c_str());
}

void updatePumpControl(uint32_t now, bool evaluateAutomaticStart) {
  if (otaInProgress || otaRebootPending) {
    writePump(false);
    return;
  }
  if (pumpRunning) {
    if (pumpElapsedMs(now) >= pumpRunMs) {
      stopPump(F("Watering complete; soil is soaking"));
    } else if (measurements.batteryVoltage < BATTERY_PUMP_CUTOFF_V) {
      stopPump(F("Pump stopped: battery voltage became too low"));
    }
    return;
  }

  if (wateringPaused(now)) {
    return;
  }

  if (!activeAutomaticWateringEnabled) {
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
  if (wifiSleeping) {
    return "Wi-Fi sleeping";
  }
  return WiFi.status() == WL_CONNECTED ? "Wi-Fi connected"
                                       : "Wi-Fi disconnected";
}

String currentIp() {
  if (wifiSleeping) {
    return F("sleeping");
  }
  return apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

bool serverSyncConfigured() {
  return SERVER_SYNC_ENABLED && strlen(SERVER_BASE_URL) > 0;
}

String serverUrl(const __FlashStringHelper* pathSuffix) {
  String base = SERVER_BASE_URL;
  if (base.endsWith("/")) {
    base.remove(base.length() - 1);
  }
  String path = String(pathSuffix);
  path.replace("{deviceId}", DEVICE_ID);
  return base + path;
}

void addServerHeaders(HTTPClient& http) {
  http.addHeader(F("Content-Type"), F("application/json"));
  if (strlen(SERVER_API_TOKEN) > 0) {
    http.addHeader(F("Authorization"), String(F("Bearer ")) + SERVER_API_TOKEN);
  }
}

String jsonStringValue(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 4);
  escaped += '"';
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c == '"' || c == '\\') {
      escaped += '\\';
    }
    escaped += c;
  }
  escaped += '"';
  return escaped;
}

bool findJsonValue(const String& json, const __FlashStringHelper* key,
                   int& valueStart) {
  const String needle = String('"') + String(key) + F("\"");
  const int keyIndex = json.indexOf(needle);
  if (keyIndex < 0) {
    return false;
  }
  const int colonIndex = json.indexOf(':', keyIndex + needle.length());
  if (colonIndex < 0) {
    return false;
  }
  valueStart = colonIndex + 1;
  while (valueStart < static_cast<int>(json.length()) &&
         isspace(json[valueStart])) {
    ++valueStart;
  }
  return valueStart < static_cast<int>(json.length());
}

bool jsonUInt(const String& json, const __FlashStringHelper* key,
              uint32_t& value) {
  int valueStart = 0;
  if (!findJsonValue(json, key, valueStart)) {
    return false;
  }
  char* end = nullptr;
  const unsigned long parsed = strtoul(json.c_str() + valueStart, &end, 10);
  if (end == json.c_str() + valueStart) {
    return false;
  }
  value = static_cast<uint32_t>(parsed);
  return true;
}

bool jsonBool(const String& json, const __FlashStringHelper* key,
              bool& value) {
  int valueStart = 0;
  if (!findJsonValue(json, key, valueStart)) {
    return false;
  }
  if (json.startsWith(F("true"), valueStart)) {
    value = true;
    return true;
  }
  if (json.startsWith(F("false"), valueStart)) {
    value = false;
    return true;
  }
  return false;
}

bool jsonString(const String& json, const __FlashStringHelper* key,
                String& value) {
  int valueStart = 0;
  if (!findJsonValue(json, key, valueStart) || json[valueStart] != '"') {
    return false;
  }
  ++valueStart;
  String parsed;
  for (int i = valueStart; i < static_cast<int>(json.length()); ++i) {
    const char c = json[i];
    if (c == '"') {
      value = parsed;
      return true;
    }
    if (c == '\\' && i + 1 < static_cast<int>(json.length())) {
      ++i;
    }
    parsed += json[i];
  }
  return false;
}

bool jsonNull(const String& json, const __FlashStringHelper* key) {
  int valueStart = 0;
  return findJsonValue(json, key, valueStart) &&
      json.startsWith(F("null"), valueStart);
}

int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097LL + static_cast<int64_t>(doe) - 719468LL;
}

bool parseUtcTimestamp(const String& value, time_t& timestamp) {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (sscanf(value.c_str(), "%d-%d-%dT%d:%d:%dZ", &year, &month, &day,
             &hour, &minute, &second) != 6) {
    return false;
  }
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 ||
      hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
    return false;
  }
  const int64_t days = daysFromCivil(year, month, day);
  timestamp = static_cast<time_t>(
      days * 86400LL + hour * 3600L + minute * 60L + second);
  return true;
}

void noteWifiActivity(uint32_t now) {
  if (!PERIODIC_WIFI_SLEEP_ENABLED || apMode) {
    return;
  }
  wifiModeChangedAt = now;
}

void sleepWifi(uint32_t now) {
  if (wifiSleeping || apMode) {
    return;
  }
  MDNS.close();
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  delay(1);
  wifiSleeping = true;
  wifiWasConnected = false;
  serverSyncSucceededThisWifiSession = false;
  wifiModeChangedAt = now;
  systemMessage = "Wi-Fi sleeping to save battery";
  Serial.println(F("[WIFI] Sleeping"));
}

void wakeWifi(uint32_t now) {
  if (!wifiSleeping) {
    return;
  }
  WiFi.forceSleepWake();
  delay(1);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  wifiSleeping = false;
  serverSyncSucceededThisWifiSession = !serverSyncConfigured();
  nextWifiConnectRetryAt = now + randomRetryDelayMs();
  nextServerSyncRetryAt = now;
  wifiModeChangedAt = now;
  systemMessage = "Wi-Fi waking";
  Serial.printf("[WIFI] Waking, connecting to %s\n", WIFI_SSID);
}

void updateWifiPower(uint32_t now) {
  if (!PERIODIC_WIFI_SLEEP_ENABLED || apMode || otaInProgress ||
      otaRebootPending) {
    return;
  }

  if (wifiSleeping) {
    if (intervalElapsed(now, wifiModeChangedAt, activeWifiOffWindowMs)) {
      wakeWifi(now);
    }
    return;
  }

  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wifiWasConnected) {
    wifiWasConnected = true;
    wifiModeChangedAt = now;
    serverSyncSucceededThisWifiSession = !serverSyncConfigured();
    nextServerSyncRetryAt = now;
    configTime(TIMEZONE, NTP_SERVER_1, NTP_SERVER_2);
    if (MDNS.begin(HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
    }
    if (serverSyncConfigured()) {
      serverSyncDue = true;
    }
    Serial.printf("[WIFI] Connected, IP=%s\n",
                  WiFi.localIP().toString().c_str());
  } else if (!connected && wifiWasConnected) {
    wifiWasConnected = false;
  }

  if (!connected && serverSyncConfigured() &&
      !serverSyncSucceededThisWifiSession) {
    if (deadlineReached(now, nextWifiConnectRetryAt)) {
      WiFi.disconnect();
      WiFi.mode(WIFI_STA);
      WiFi.hostname(HOSTNAME);
      WiFi.setAutoReconnect(true);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      nextWifiConnectRetryAt = now + randomRetryDelayMs();
      systemMessage = "Retrying Wi-Fi for server sync";
      Serial.println(F("[WIFI] Retrying Wi-Fi connection for server sync"));
    }
    return;
  }

  if (connected && serverSyncConfigured() &&
      !serverSyncSucceededThisWifiSession) {
    return;
  }

  const uint32_t window = connected ? activeWifiOnWindowMs
                                    : WIFI_CONNECT_TIMEOUT_MS;
  if (intervalElapsed(now, wifiModeChangedAt, window)) {
    sleepWifi(now);
  }
}

uint32_t wifiNextTransitionMs(uint32_t now) {
  if (!PERIODIC_WIFI_SLEEP_ENABLED || apMode) {
    return 0;
  }
  const uint32_t window = wifiSleeping
      ? activeWifiOffWindowMs
      : (WiFi.status() == WL_CONNECTED ? activeWifiOnWindowMs
                                       : WIFI_CONNECT_TIMEOUT_MS);
  const uint32_t elapsed = static_cast<uint32_t>(now - wifiModeChangedAt);
  return elapsed >= window ? 0 : window - elapsed;
}

String telemetryJson(uint32_t now) {
  const uint32_t pauseRemaining = wateringPauseRemainingMs(now);
  const String timestamp = formattedUtcDateTime();
  const String currentDateTime = formattedDateTime();
  String json;
  json.reserve(760);
  json += F("{\"deviceId\":");
  json += jsonStringValue(DEVICE_ID);
  json += F(",\"timestamp\":");
  json += timestamp.length() == 0 ? F("null") : jsonStringValue(timestamp);
  json += F(",\"currentDateTime\":");
  json += currentDateTime.length() == 0 ? F("null") : jsonStringValue(currentDateTime);
  json += F(",\"soilPercent\":");
  json += measurements.soilPercent;
  json += F(",\"soilRaw\":");
  json += measurements.soilRaw;
  json += F(",\"soilValid\":");
  json += measurements.soilValid ? F("true") : F("false");
  json += F(",\"batteryVoltage\":");
  json += String(measurements.batteryVoltage, 3);
  json += F(",\"batteryPercent\":");
  json += measurements.batteryPercent;
  json += F(",\"batteryRaw\":");
  json += measurements.batteryRaw;
  json += F(",\"pumpRunning\":");
  json += pumpRunning ? F("true") : F("false");
  json += F(",\"pumpDurationMs\":");
  json += pumpRunMs;
  json += F(",\"pumpRemainingMs\":");
  json += pumpRemainingMs(now);
  json += F(",\"wateringRunCount\":");
  json += wateringRunCount;
  json += F(",\"wateringPaused\":");
  json += pauseRemaining > 0 ? F("true") : F("false");
  json += F(",\"wateringPauseUntil\":");
  json += wateringPausedUntil > 0 ? jsonStringValue(formattedUtcDateTime(wateringPausedUntil)) : F("null");
  json += F(",\"controlMode\":");
  json += jsonStringValue(String(controlModeName()));
  json += F(",\"wifiRssi\":");
  json += WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : F("null");
  json += F(",\"uptimeMs\":");
  json += now;
  json += F(",\"firmwareVersion\":");
  json += jsonStringValue(FIRMWARE_VERSION);
  json += F("}");
  return json;
}

bool postTelemetryToServer(uint32_t now) {
  WiFiClient client;
  HTTPClient http;
  const String url = serverUrl(F("/api/devices/{deviceId}/telemetry"));
  if (!http.begin(client, url)) {
    lastServerTelemetryStatus = -1;
    lastServerSyncMessage = "Telemetry begin failed";
    return false;
  }
  http.setTimeout(SERVER_HTTP_TIMEOUT_MS);
  addServerHeaders(http);
  const String payload = telemetryJson(now);
  lastServerTelemetryStatus = http.POST(payload);
  lastServerSyncMessage = lastServerTelemetryStatus > 0
      ? "Telemetry POST status " + String(lastServerTelemetryStatus)
      : "Telemetry POST failed " + String(lastServerTelemetryStatus);
  http.end();
  yield();
  return lastServerTelemetryStatus >= 200 && lastServerTelemetryStatus < 300;
}

bool applyServerConfig(const String& body) {
  uint32_t version = 0;
  if (!jsonUInt(body, F("version"), version) ||
      version <= serverConfigVersion) {
    return false;
  }

  uint32_t value = 0;
  if (jsonUInt(body, F("pumpDurationMs"), value) &&
      value >= PUMP_RUN_MIN_MS && value <= PUMP_RUN_MAX_MS) {
    pumpRunMs = value;
  }
  if (jsonUInt(body, F("soilStartWateringPercent"), value)) {
    activeSoilStartWateringPercent =
        static_cast<uint8_t>(clampValue<uint32_t>(value, 0, 100));
  }
  if (jsonUInt(body, F("wateringCooldownMs"), value)) {
    activeWateringCooldownMs = value;
  }
  if (jsonUInt(body, F("sensorIntervalMs"), value) && value >= 1000UL) {
    activeSensorIntervalMs = value;
  }
  if (jsonUInt(body, F("wifiOnWindowMs"), value) && value >= 1000UL) {
    activeWifiOnWindowMs = value;
  }
  if (jsonUInt(body, F("wifiOffWindowMs"), value) && value >= 1000UL) {
    activeWifiOffWindowMs = value;
  }
  if (jsonUInt(body, F("oledOnDurationMs"), value)) {
    activeOledIdleTimeoutMs = value;
  }
  bool boolValue = false;
  if (jsonBool(body, F("automaticWateringEnabled"), boolValue)) {
    activeAutomaticWateringEnabled = boolValue;
  }
  if (jsonNull(body, F("wateringPausedUntil"))) {
    wateringPausedUntil = 0;
    pauseDurationMs = 0;
  } else {
    String pauseUntil;
    time_t parsedPauseUntil = 0;
    if (jsonString(body, F("wateringPausedUntil"), pauseUntil) &&
        parseUtcTimestamp(pauseUntil, parsedPauseUntil)) {
      wateringPausedUntil = parsedPauseUntil;
      pauseDurationMs = 0;
    }
  }

  serverConfigVersion = version;
  saveRtcState();
  savePersistentState();
  systemMessage = "Applied server config v" + String(serverConfigVersion);
  Serial.printf("[SERVER] Applied config version %lu\n",
                static_cast<unsigned long>(serverConfigVersion));
  return true;
}

bool pullConfigFromServer() {
  WiFiClient client;
  HTTPClient http;
  const String url = serverUrl(F("/api/devices/{deviceId}/config"));
  if (!http.begin(client, url)) {
    lastServerConfigStatus = -1;
    lastServerSyncMessage = "Config begin failed";
    return false;
  }
  http.setTimeout(SERVER_HTTP_TIMEOUT_MS);
  addServerHeaders(http);
  lastServerConfigStatus = http.GET();
  if (lastServerConfigStatus == HTTP_CODE_OK) {
    const String body = http.getString();
    const bool applied = applyServerConfig(body);
    lastServerSyncMessage = applied
        ? "Config applied v" + String(serverConfigVersion)
        : "Config unchanged v" + String(serverConfigVersion);
  } else {
    lastServerSyncMessage = "Config GET status " + String(lastServerConfigStatus);
  }
  http.end();
  yield();
  return lastServerConfigStatus == HTTP_CODE_OK;
}

bool checkOtaManifestFromServer() {
  WiFiClient client;
  HTTPClient http;
  const String url = serverUrl(F("/api/devices/{deviceId}/ota"));
  if (!http.begin(client, url)) {
    lastServerOtaStatus = -1;
    lastServerSyncMessage = "OTA begin failed";
    return false;
  }
  http.setTimeout(SERVER_HTTP_TIMEOUT_MS);
  addServerHeaders(http);
  lastServerOtaStatus = http.GET();
  if (lastServerOtaStatus == HTTP_CODE_OK) {
    const String body = http.getString();
    bool updateAvailable = false;
    serverOtaUpdateAvailable =
        jsonBool(body, F("updateAvailable"), updateAvailable) && updateAvailable;
    serverOtaVersion = String();
    serverOtaUrl = String();
    serverOtaSha256 = String();
    serverOtaSize = 0;
    if (serverOtaUpdateAvailable) {
      jsonString(body, F("version"), serverOtaVersion);
      jsonString(body, F("firmwareUrl"), serverOtaUrl);
      jsonString(body, F("sha256"), serverOtaSha256);
      jsonUInt(body, F("size"), serverOtaSize);
      lastServerSyncMessage = "OTA available " + serverOtaVersion;
      Serial.printf("[SERVER] OTA available version=%s size=%lu url=%s\n",
                    serverOtaVersion.c_str(),
                    static_cast<unsigned long>(serverOtaSize),
                    serverOtaUrl.c_str());
    }
  } else {
    lastServerSyncMessage = "OTA GET status " + String(lastServerOtaStatus);
  }
  http.end();
  yield();
  return lastServerOtaStatus == HTTP_CODE_OK;
}

void scheduleServerSyncRetry(uint32_t now) {
  const uint32_t delayMs = randomRetryDelayMs();
  nextServerSyncRetryAt = now + delayMs;
  serverSyncDue = false;
  systemMessage = "Server retry in " + formattedDuration(delayMs);
  Serial.printf("[SERVER] Sync failed; retry in %lu ms\n",
                static_cast<unsigned long>(delayMs));
}

bool syncWithServer(uint32_t now, bool force = false) {
  if (!serverSyncConfigured()) {
    lastServerSyncMessage = "Server sync disabled";
    serverSyncSucceededThisWifiSession = true;
    return false;
  }
  if (wifiSleeping || apMode || WiFi.status() != WL_CONNECTED) {
    return false;
  }
  if (pumpRunning || otaInProgress || otaRebootPending) {
    return false;
  }
  if (!force &&
      !intervalElapsed(now, lastServerSyncAt, SERVER_SYNC_INTERVAL_MS)) {
    return false;
  }
  lastServerSyncAt = now;
  bool configOk = false;
  bool otaOk = false;
  bool telemetryOk = false;
  if (!pumpRunning && !otaInProgress && !otaRebootPending) {
    configOk = pullConfigFromServer();
  }
  if (!pumpRunning && !otaInProgress && !otaRebootPending) {
    otaOk = checkOtaManifestFromServer();
  }
  if (!pumpRunning && !otaInProgress && !otaRebootPending) {
    telemetryOk = postTelemetryToServer(millis());
  }
  const bool success = configOk && otaOk && telemetryOk;
  if (!success) {
    serverSyncSucceededThisWifiSession = false;
    scheduleServerSyncRetry(millis());
    return false;
  }

  serverSyncSucceededThisWifiSession = true;
  lastSuccessfulServerSyncAt = millis();
  lastServerSyncMessage = "Server sync complete";
  if (PERIODIC_WIFI_SLEEP_ENABLED && !pumpRunning && !otaInProgress &&
      !otaRebootPending && !apMode && !wifiSleeping) {
    sleepWifi(millis());
  }
  return true;
}

String statusJson() {
  const uint32_t now = millis();
  const uint32_t nextScheduledMs = scheduleRemainingMs(now);
  const uint32_t pauseRemaining = wateringPauseRemainingMs(now);
  String nextScheduledDateTime;
  if (timeIsSynced() && scheduleFallbackActive()) {
    nextScheduledDateTime =
        formattedDateTime(time(nullptr) + ((nextScheduledMs + 999U) / 1000U));
  }
  String json;
  json.reserve(720);
  json += F("{\"wifi\":\"");
  json += wifiDescription();
  json += F("\",\"wifiSleeping\":");
  json += wifiSleeping ? F("true") : F("false");
  json += F(",\"wifiNextTransitionMs\":");
  json += wifiNextTransitionMs(now);
  json += F(",\"serverSyncEnabled\":");
  json += serverSyncConfigured() ? F("true") : F("false");
  json += F(",\"serverSyncSucceededThisWifiSession\":");
  json += serverSyncSucceededThisWifiSession ? F("true") : F("false");
  json += F(",\"serverSyncRetryInMs\":");
  json += deadlineReached(now, nextServerSyncRetryAt)
      ? 0
      : static_cast<uint32_t>(nextServerSyncRetryAt - now);
  json += F(",\"lastServerTelemetryStatus\":");
  json += lastServerTelemetryStatus;
  json += F(",\"lastServerConfigStatus\":");
  json += lastServerConfigStatus;
  json += F(",\"lastServerOtaStatus\":");
  json += lastServerOtaStatus;
  json += F(",\"serverConfigVersion\":");
  json += serverConfigVersion;
  json += F(",\"serverOtaUpdateAvailable\":");
  json += serverOtaUpdateAvailable ? F("true") : F("false");
  json += F(",\"serverOtaVersion\":");
  json += jsonStringValue(serverOtaVersion);
  json += F(",\"serverOtaSize\":");
  json += serverOtaSize;
  json += F(",\"lastServerSyncMessage\":");
  json += jsonStringValue(lastServerSyncMessage);
  json += F(",\"ip\":\"");
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
  json += pumpRunMs;
  json += F(",\"soilStartWateringPercent\":");
  json += activeSoilStartWateringPercent;
  json += F(",\"wateringCooldownMs\":");
  json += activeWateringCooldownMs;
  json += F(",\"sensorIntervalMs\":");
  json += activeSensorIntervalMs;
  json += F(",\"wifiOnWindowMs\":");
  json += activeWifiOnWindowMs;
  json += F(",\"wifiOffWindowMs\":");
  json += activeWifiOffWindowMs;
  json += F(",\"oledOnDurationMs\":");
  json += activeOledIdleTimeoutMs;
  json += F(",\"wateringRunCount\":");
  json += wateringRunCount;
  json += F(",\"cooldownRemainingMs\":");
  json += cooldownRemainingMs(now);
  json += F(",\"startupPumpHoldRemainingMs\":");
  json += startupPumpHoldRemainingMs(now);
  json += F(",\"wateringPaused\":");
  json += pauseRemaining > 0 ? F("true") : F("false");
  json += F(",\"wateringPauseRemainingMs\":");
  json += pauseRemaining == UINT32_MAX ? 0 : pauseRemaining;
  json += F(",\"wateringPauseUntil\":\"");
  json += wateringPausedUntil > 0 ? formattedDateTime(wateringPausedUntil)
                                  : String();
  json += F("\"");
  json += F(",\"automaticEnabled\":");
  json += activeAutomaticWateringEnabled ? F("true") : F("false");
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

String prometheusMetrics() {
  const uint32_t now = millis();
  const uint32_t pauseRemaining = wateringPauseRemainingMs(now);
  String metrics;
  metrics.reserve(1400);
  metrics += F("# TYPE plant_soil_moisture_percent gauge\nplant_soil_moisture_percent ");
  metrics += measurements.soilPercent;
  metrics += '\n';
  metrics += F("# TYPE plant_soil_raw gauge\nplant_soil_raw ");
  metrics += measurements.soilRaw;
  metrics += '\n';
  metrics += F("# TYPE plant_soil_sensor_valid gauge\nplant_soil_sensor_valid ");
  metrics += measurements.soilValid ? '1' : '0';
  metrics += '\n';
  metrics += F("# TYPE plant_battery_voltage_volts gauge\nplant_battery_voltage_volts ");
  metrics += String(measurements.batteryVoltage, 3);
  metrics += '\n';
  metrics += F("# TYPE plant_battery_charge_percent gauge\nplant_battery_charge_percent ");
  metrics += measurements.batteryPercent;
  metrics += '\n';
  metrics += F("# TYPE plant_pump_running gauge\nplant_pump_running ");
  metrics += pumpRunning ? '1' : '0';
  metrics += '\n';
  metrics += F("# TYPE plant_watering_runs_total counter\nplant_watering_runs_total ");
  metrics += wateringRunCount;
  metrics += '\n';
  metrics += F("# TYPE plant_watering_paused gauge\nplant_watering_paused ");
  metrics += pauseRemaining > 0 ? '1' : '0';
  metrics += '\n';
  metrics += F("# TYPE plant_watering_pause_remaining_seconds gauge\nplant_watering_pause_remaining_seconds ");
  metrics += pauseRemaining == UINT32_MAX ? 0 : pauseRemaining / 1000U;
  metrics += '\n';
  metrics += F("# TYPE plant_pump_remaining_seconds gauge\nplant_pump_remaining_seconds ");
  metrics += pumpRemainingMs(now) / 1000U;
  metrics += '\n';
  metrics += F("# TYPE plant_pump_duration_seconds gauge\nplant_pump_duration_seconds ");
  metrics += pumpRunMs / 1000U;
  metrics += '\n';
  metrics += F("# TYPE plant_cooldown_remaining_seconds gauge\nplant_cooldown_remaining_seconds ");
  metrics += cooldownRemainingMs(now) / 1000U;
  metrics += '\n';
  metrics += F("# TYPE plant_controller_uptime_seconds counter\nplant_controller_uptime_seconds ");
  metrics += now / 1000U;
  metrics += '\n';
  metrics += F("# TYPE plant_wifi_sleeping gauge\nplant_wifi_sleeping ");
  metrics += wifiSleeping ? '1' : '0';
  metrics += '\n';
  metrics += F("# TYPE plant_wifi_next_transition_seconds gauge\nplant_wifi_next_transition_seconds ");
  metrics += wifiNextTransitionMs(now) / 1000U;
  metrics += '\n';
  metrics += F("# TYPE plant_server_config_version gauge\nplant_server_config_version ");
  metrics += serverConfigVersion;
  metrics += '\n';
  metrics += F("# TYPE plant_server_ota_update_available gauge\nplant_server_ota_update_available ");
  metrics += serverOtaUpdateAvailable ? '1' : '0';
  metrics += '\n';
  return metrics;
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    const uint32_t now = millis();
    wakeDisplay(now);
    noteWifiActivity(now);
    server.send_P(200, "text/html", DASHBOARD_HTML);
  });
  server.on("/api/status", HTTP_GET, []() {
    server.send(200, "application/json", statusJson());
  });
  server.on("/metrics", HTTP_GET, []() {
    server.send(200, "text/plain; version=0.0.4; charset=utf-8",
                prometheusMetrics());
  });
  server.on("/pump", HTTP_POST, []() {
    const uint32_t now = millis();
    wakeDisplay(now);
    noteWifiActivity(now);
    const bool started = startPump(PumpSource::Manual);
    String response = F("{\"started\":");
    response += started ? F("true") : F("false");
    response += F(",\"message\":\"");
    response += systemMessage;
    response += F("\"}");
    server.send(started ? 202 : 409, "application/json", response);
  });
  server.on("/pump/stop", HTTP_POST, []() {
    const uint32_t now = millis();
    wakeDisplay(now);
    noteWifiActivity(now);
    const bool wasRunning = pumpRunning;
    if (wasRunning) {
      stopPump(F("Watering stopped manually"));
    } else {
      systemMessage = "Pump is already stopped";
    }
    String response = F("{\"stopped\":");
    response += wasRunning ? F("true") : F("false");
    response += F(",\"message\":\"");
    response += systemMessage;
    response += F("\"}");
    server.send(wasRunning ? 200 : 409, "application/json", response);
  });
  server.on("/pause", HTTP_POST, []() {
    const uint32_t now = millis();
    wakeDisplay(now);
    noteWifiActivity(now);
    const int hours = static_cast<int>(
        clampValue(server.arg("hours").toInt(), 0L, 24L));
    if (pumpRunning) {
      stopPump(F("Watering stopped and paused"));
    }
    if (hours == 0) {
      wateringPausedUntil = 0;
      pauseDurationMs = 0;
      systemMessage = "Watering pause cleared";
    } else if (timeIsSynced()) {
      wateringPausedUntil = time(nullptr) + hours * 3600L;
      pauseDurationMs = 0;
      systemMessage = "Watering paused for " + String(hours) + " hours";
    } else {
      wateringPausedUntil = 0;
      pauseStartedAt = millis();
      pauseDurationMs = static_cast<uint32_t>(hours) * 60UL * 60UL * 1000UL;
      systemMessage = "Watering paused for " + String(hours) + " hours";
    }
    saveRtcState();
    savePersistentState();
    server.send(200, "application/json", statusJson());
  });
  server.on("/settings/pump-duration", HTTP_POST, []() {
    const uint32_t now = millis();
    wakeDisplay(now);
    noteWifiActivity(now);
    const long requestedSeconds = server.arg("seconds").toInt();
    const long minSeconds = PUMP_RUN_MIN_MS / 1000UL;
    const long maxSeconds = PUMP_RUN_MAX_MS / 1000UL;
    if (requestedSeconds < minSeconds || requestedSeconds > maxSeconds) {
      server.send(400, "application/json",
                  "{\"error\":\"Duration must be between 5 and 300 seconds\"}");
      return;
    }
    if (pumpRunning) {
      server.send(409, "application/json",
                  "{\"error\":\"Stop the pump before changing duration\"}");
      return;
    }
    pumpRunMs = static_cast<uint32_t>(requestedSeconds) * 1000UL;
    savePersistentState();
    systemMessage = "Pump duration set to " + String(requestedSeconds) +
        " seconds";
    server.send(200, "application/json", statusJson());
  });
  server.on("/update", HTTP_GET, []() {
    const uint32_t now = millis();
    wakeDisplay(now);
    noteWifiActivity(now);
    if (!server.authenticate(OTA_USERNAME, OTA_PASSWORD)) {
      server.requestAuthentication();
      return;
    }
    server.send_P(200, "text/html", UPDATE_HTML);
  });
  server.on(
      "/update", HTTP_POST,
      []() {
        if (!server.authenticate(OTA_USERNAME, OTA_PASSWORD)) {
          server.requestAuthentication();
          return;
        }

        otaInProgress = false;
        if (otaUploadRejected) {
          server.send(pumpRunning ? 409 : 400, "text/plain",
                      pumpRunning ? "Stop watering before updating."
                                  : "Firmware upload failed. Check serial output.");
          return;
        }
        if (Update.hasError()) {
          server.send(500, "text/plain",
                      "Firmware validation or flash write failed.");
          return;
        }

        systemMessage = "Firmware updated; restarting";
        wakeDisplay(millis());
        server.send(200, "text/html",
                    "<h1>Update complete</h1><p>The controller is restarting.</p>");
        otaRebootPending = true;
        otaRebootAt = millis();
        Serial.println(F("[OTA] Update complete; restart scheduled"));
      },
      []() {
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
          otaUploadRejected =
              !server.authenticate(OTA_USERNAME, OTA_PASSWORD) || pumpRunning;
          if (otaUploadRejected) {
            Serial.println(pumpRunning
                               ? F("[OTA] Rejected while pump is running")
                               : F("[OTA] Rejected unauthenticated upload"));
            return;
          }

          writePump(false);
          otaInProgress = true;
          systemMessage = "Firmware update in progress";
          wakeDisplay(millis());
          const uint32_t maxSketchSpace =
              (ESP.getFreeSketchSpace() - 0x1000U) & 0xFFFFF000U;
          Serial.printf("[OTA] Upload started: %s, capacity=%lu bytes\n",
                        upload.filename.c_str(),
                        static_cast<unsigned long>(maxSketchSpace));
          if (!Update.begin(maxSketchSpace, U_FLASH)) {
            Update.printError(Serial);
            otaUploadRejected = true;
            otaInProgress = false;
          }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (!otaUploadRejected &&
              Update.write(upload.buf, upload.currentSize) !=
                  upload.currentSize) {
            Update.printError(Serial);
            otaUploadRejected = true;
          }
        } else if (upload.status == UPLOAD_FILE_END) {
          if (!otaUploadRejected && !Update.end(true)) {
            Update.printError(Serial);
            otaUploadRejected = true;
          }
          otaInProgress = false;
          Serial.printf("[OTA] Received %lu bytes, result=%s\n",
                        static_cast<unsigned long>(upload.totalSize),
                        otaUploadRejected ? "failed" : "success");
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
          Update.end();
          otaUploadRejected = true;
          otaInProgress = false;
          systemMessage = "Firmware update aborted";
          Serial.println(F("[OTA] Upload aborted"));
        }
        yield();
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
  wakeDisplay(millis());
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
    wifiSleeping = false;
    wifiWasConnected = true;
    wifiModeChangedAt = millis();
    serverSyncSucceededThisWifiSession = !serverSyncConfigured();
    nextServerSyncRetryAt = wifiModeChangedAt;
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

  if (serverSyncConfigured()) {
    apMode = false;
    wifiSleeping = false;
    wifiWasConnected = false;
    wifiModeChangedAt = millis();
    serverSyncSucceededThisWifiSession = false;
    nextWifiConnectRetryAt = wifiModeChangedAt + randomRetryDelayMs();
    nextServerSyncRetryAt = wifiModeChangedAt;
    systemMessage = "Wi-Fi retrying for server";
    Serial.println(F("[WIFI] Initial connection failed; keeping station mode for server retry"));
    showStartupMessage(F("Wi-Fi retrying"), F("Waiting server"));
    return;
  }

  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  apMode = WiFi.softAP("Plant-Watering-Setup", AP_PASSWORD);
  wifiSleeping = false;
  wifiWasConnected = false;
  wifiModeChangedAt = millis();
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

  if (pumpRunning || otaInProgress || otaRebootPending) {
    wakeDisplay(now);
  }
  if (displayOn && activeOledIdleTimeoutMs > 0 &&
      intervalElapsed(now, lastOledActivityAt, activeOledIdleTimeoutMs)) {
    sleepDisplay();
    return;
  }
  if (!displayOn) {
    return;
  }

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println(formattedOledDateTime());
  display.print(F("FW "));
  display.print(FIRMWARE_VERSION);
  display.print(F(" Sync "));
  display.println(formattedServerSyncAge(now));
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
    display.println(formattedDuration(pumpRunMs));
    display.print(F("Left "));
    display.println(formattedDuration(pumpRemainingMs(now)));
  } else {
    display.print(F("Pump OFF Runs:"));
    display.println(wateringRunCount);
    const uint32_t startupHold = startupPumpHoldRemainingMs(now);
    const uint32_t pauseRemaining = wateringPauseRemainingMs(now);
    const uint32_t scheduled = scheduleRemainingMs(now);
    const uint32_t cooldown = cooldownRemainingMs(now);
    if (pauseRemaining > 0) {
      display.print(F("Paused "));
      display.println(pauseRemaining == UINT32_MAX
                          ? String(F("syncing"))
                          : formattedHoursMinutes(pauseRemaining));
    } else if (startupHold > 0) {
      display.print(F("Boot hold "));
      display.println(formattedDuration(startupHold));
    } else if (scheduleFallbackActive()) {
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

void enterDeepSleepIfReady(uint32_t now) {
  if (!DEEP_SLEEP_ENABLED || pumpRunning || otaInProgress ||
      otaRebootPending || apMode || !timeIsSynced() ||
      now < AWAKE_WINDOW_MS) {
    return;
  }
  writePump(false);
  setRgb(false, false, false);
  saveRtcState();
  savePersistentState();
  showStartupMessage(F("Deep sleep"), F("Wake in 10 min"));
  delay(100);
  if (displayReady) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
  }
  WiFi.disconnect(true);
  delay(100);
  ESP.deepSleep(DEEP_SLEEP_DURATION_US, WAKE_RF_DEFAULT);
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
  randomSeed(ESP.getCycleCount() ^ ESP.getChipId() ^ micros());
  wokeFromDeepSleep = ESP.getResetReason().indexOf(F("Deep-Sleep")) >= 0;
  loadRtcState();
  loadPersistentState();

  pinMode(PUMP_PIN, OUTPUT);
  writePump(false);
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  if (!DEEP_SLEEP_ENABLED) {
    pinMode(LED_BLUE_PIN, OUTPUT);
  }
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
    displayOn = true;
    lastOledActivityAt = millis();
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
  if (serverSyncConfigured() && WiFi.status() == WL_CONNECTED) {
    serverSyncDue = true;
  }
  setupWebServer();
  delay(500);
}

void loop() {
  uint32_t now = millis();
  bool sensorSampleTaken = false;
  updateWifiPower(now);
  if (!wifiSleeping) {
    server.handleClient();
  }
  now = millis();
  if (otaRebootPending && intervalElapsed(now, otaRebootAt, 1000)) {
    writePump(false);
    Serial.println(F("[OTA] Restarting"));
    Serial.flush();
    ESP.restart();
  }
  if (!wifiSleeping && !apMode && WiFi.status() == WL_CONNECTED) {
    MDNS.update();
  }

  if (serverSyncConfigured() && !serverSyncSucceededThisWifiSession &&
      !wifiSleeping && !apMode && WiFi.status() == WL_CONNECTED &&
      deadlineReached(now, nextServerSyncRetryAt)) {
    serverSyncDue = true;
  }

  const bool forceServerSync = serverSyncDue && serverSyncConfigured() &&
      !wifiSleeping && !apMode && WiFi.status() == WL_CONNECTED &&
      !pumpRunning && !otaInProgress && !otaRebootPending;
  if (forceServerSync) {
    serverSyncDue = false;
    lastSensorAt = now;
    readSensors();
    sensorSampleTaken = true;
  } else if (intervalElapsed(now, lastSensorAt, activeSensorIntervalMs)) {
    lastSensorAt = now;
    readSensors();
    sensorSampleTaken = true;
  }

  if (sensorSampleTaken) {
    syncWithServer(now, forceServerSync);
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

  if (intervalElapsed(now, lastPersistentSaveAt,
                      PERSISTENT_STATE_SAVE_INTERVAL_MS)) {
    savePersistentState();
  }

  enterDeepSleepIfReady(now);

  yield();
}
