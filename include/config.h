#pragma once

#include "secrets.h"

// Wi-Fi credentials. If connection fails, the controller starts its own
// "Plant-Watering-Setup" access point using AP_PASSWORD.
constexpr char AP_PASSWORD[] = "waterplant";
constexpr char HOSTNAME[] = "plant-water";

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
constexpr float BATTERY_DIVIDER_RATIO = 2.00f;
constexpr float BATTERY_EMPTY_V = 3.20f;
constexpr float BATTERY_FULL_V = 4.20f;
constexpr float BATTERY_PUMP_CUTOFF_V = 3.30f;

// Watering behavior.
constexpr uint32_t PUMP_RUN_MS = 5UL * 60UL * 1000UL;
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
constexpr uint32_t SENSOR_INTERVAL_MS = 2000;
constexpr uint32_t DISPLAY_INTERVAL_MS = 250;
constexpr uint32_t SERIAL_INTERVAL_MS = 5000;
