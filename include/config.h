#pragma once

#include "secrets.h"

// Wi-Fi credentials. If connection fails, the controller starts its own
// "Plant-Watering-Setup" access point using AP_PASSWORD.
constexpr char AP_PASSWORD[] = "waterplant";
constexpr char HOSTNAME[] = "plant-water";

// Server sync identity and defaults. Override these macros in secrets.h for a
// real deployment; sync remains disabled unless SERVER_SYNC_ENABLED is true.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.1.0"
#endif
#ifndef DEVICE_ID
#define DEVICE_ID "plant-water-01"
#endif
#ifndef SERVER_BASE_URL
#define SERVER_BASE_URL ""
#endif
#ifndef SERVER_API_TOKEN
#define SERVER_API_TOKEN ""
#endif
#ifndef SERVER_SYNC_ENABLED
#define SERVER_SYNC_ENABLED false
#endif
#ifndef SERVER_SYNC_INTERVAL_MS
#define SERVER_SYNC_INTERVAL_MS (10UL * 60UL * 1000UL)
#endif
#ifndef SERVER_HTTP_TIMEOUT_MS
#define SERVER_HTTP_TIMEOUT_MS 8000
#endif

// NTP and local timezone. This default observes US Eastern EST/EDT.
constexpr char TIMEZONE[] = "EST5EDT,M3.2.0,M11.1.0";
constexpr char NTP_SERVER_1[] = "pool.ntp.org";
constexpr char NTP_SERVER_2[] = "time.nist.gov";

// Match the proven sketch's Wire.begin(14, 12): SDA=GPIO14/D5 and
// SCL=GPIO12/D6. The ADS1115 shares the same I2C bus.
constexpr uint8_t I2C_SDA_PIN = D5;
constexpr uint8_t I2C_SCL_PIN = D6;
constexpr uint8_t OLED_PREFERRED_ADDRESS = 0x3C;
constexpr bool OLED_AUTO_DETECT_ADDRESS = true;
constexpr uint8_t OLED_WIDTH = 128;
constexpr uint8_t OLED_HEIGHT = 64;
constexpr uint8_t OLED_ROTATION = 0;
// Set to a GPIO only if the display's RESET pin is connected to the ESP8266.
constexpr int8_t OLED_RESET_PIN = -1;
constexpr uint32_t I2C_CLOCK_STRETCH_LIMIT_US = 2000;
constexpr uint8_t ADS1115_ADDRESS = 0x48;
constexpr uint8_t SOIL_ADS_CHANNEL = 0;

// Outputs.
constexpr uint8_t PUMP_PIN = D1;
constexpr uint8_t LED_RED_PIN = D2;
constexpr uint8_t LED_GREEN_PIN = D7;
constexpr uint8_t LED_BLUE_PIN = D0;

// Change these if your relay module or RGB LED uses inverted logic.
constexpr bool PUMP_ACTIVE_HIGH = true;
constexpr bool LED_COMMON_ANODE = false;

// Soil calibration: record ADS1115 readings in dry air/soil and fully wet
// soil, then replace these defaults. Either value may be the larger one.
constexpr int16_t SOIL_DRY_RAW = 22000;
constexpr int16_t SOIL_WET_RAW = 9000;
constexpr uint8_t SOIL_START_WATERING_PERCENT = 30;
constexpr uint8_t SOIL_DRY_CONFIRMATION_SAMPLES = 3;

// A0 battery calibration. On a NodeMCU, A0 is commonly scaled to about 3.3 V.
// A bare ESP8266 ADC accepts only 1.0 V. Verify your exact board and divider.
// Divider ratio = (top resistor + bottom resistor) / bottom resistor.
constexpr float BATTERY_ADC_FULL_SCALE_V = 3.30f;
// Installed divider: 47 kOhm from battery+ to A0 and 100 kOhm from A0 to GND.
constexpr float BATTERY_DIVIDER_RATIO = 1.47f;
constexpr float BATTERY_EMPTY_V = 3.20f;
constexpr float BATTERY_FULL_V = 4.20f;
constexpr float BATTERY_PUMP_CUTOFF_V = 3.30f;

// Watering behavior.
constexpr uint32_t STARTUP_PUMP_HOLD_MS = 2UL * 60UL * 1000UL;
constexpr uint32_t PUMP_RUN_DEFAULT_MS = 30UL * 1000UL;
constexpr uint32_t PUMP_RUN_MIN_MS = 5UL * 1000UL;
constexpr uint32_t PUMP_RUN_MAX_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t WATERING_COOLDOWN_MS = 30UL * 60UL * 1000UL;
constexpr bool AUTOMATIC_WATERING_ENABLED = true;

// Used only while the ADS1115 or soil reading is unavailable. This is an
// interval schedule based on uptime, not a wall-clock schedule.
constexpr bool SENSORLESS_SCHEDULE_ENABLED = true;
constexpr uint32_t SENSORLESS_FIRST_RUN_DELAY_MS =
    12UL * 60UL * 60UL * 1000UL;
constexpr uint32_t SENSORLESS_WATERING_INTERVAL_MS =
    12UL * 60UL * 60UL * 1000UL;

// Update rates.
constexpr uint32_t SENSOR_INTERVAL_MS = 10UL * 60UL * 1000UL;
constexpr uint32_t DISPLAY_INTERVAL_MS = 250;
constexpr uint32_t OLED_IDLE_TIMEOUT_MS = 60UL * 1000UL;
constexpr uint32_t PERSISTENT_STATE_SAVE_INTERVAL_MS = 60UL * 1000UL;
constexpr uint32_t SERIAL_INTERVAL_MS = 5000;

// Periodically turn Wi-Fi off to reduce battery use. Sensors, pump control,
// OLED, and serial continue working while Wi-Fi is off. Web, OTA, and
// Prometheus are available only during the Wi-Fi-on window.
constexpr bool PERIODIC_WIFI_SLEEP_ENABLED = true;
constexpr uint32_t WIFI_ON_WINDOW_MS = 2UL * 60UL * 1000UL;
constexpr uint32_t WIFI_OFF_WINDOW_MS = 10UL * 60UL * 1000UL;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20UL * 1000UL;

// Requires a physical D0/GPIO16-to-RST wire. D0 cannot also drive the blue
// LED. HTTP endpoints are available only during the awake window.
constexpr bool DEEP_SLEEP_ENABLED = false;
constexpr uint32_t AWAKE_WINDOW_MS = 60UL * 1000UL;
constexpr uint64_t DEEP_SLEEP_DURATION_US = 10ULL * 60ULL * 1000000ULL;
