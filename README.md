# ESP8266 Plant Watering Controller

Automatic plant watering firmware for an ESP8266 with:

- SSD1306 128x64 OLED
- ADS1115 and capacitive soil moisture sensor
- Battery voltage monitoring on `A0`
- Pump driver and RGB status LED
- Live web dashboard with manual watering
- NTP date/time and serial diagnostics
- Scheduled watering fallback when soil sensing is unavailable

## Hardware

The ESP8266 has one analog input. This project reserves `A0` for battery
monitoring and reads the analog soil sensor through an ADS1115.

| Device | Connection | ESP8266 |
|---|---|---|
| SSD1306 | SDA | D5 / GPIO14 |
| SSD1306 | SCL | D6 / GPIO12 |
| ADS1115 | SDA | D5 / GPIO14 |
| ADS1115 | SCL | D6 / GPIO12 |
| ADS1115 | ADDR | GND (`0x48`) |
| Soil sensor | AOUT | ADS1115 A0 |
| Soil sensor | VCC | 3.3 V |
| Battery divider | Output | A0 |
| Pump driver | Signal | D1 / GPIO5 |
| RGB LED | Red | D2 / GPIO4 through resistor |
| RGB LED | Green | D7 / GPIO13 through resistor |
| RGB LED | Blue | D0 / GPIO16 through resistor |

Connect all logic grounds together.

Do not drive a pump directly from an ESP8266 pin. Use a logic-level MOSFET or
relay module, a flyback diode for a DC motor pump, a gate/pulldown resistor,
and a correctly rated pump power supply.

Do not connect a battery directly to `A0` unless its maximum voltage is safe
for the exact board. A bare ESP8266 ADC accepts 1.0 V; many NodeMCU boards add
an onboard divider for an approximately 3.3 V input range.

## Setup

1. Install [PlatformIO](https://platformio.org/).
2. Create `include/secrets.h` from `include/secrets.example.h`.
3. Set `WIFI_SSID` and `WIFI_PASSWORD` in `include/secrets.h`.
4. Review hardware and watering settings in `include/config.h`.
5. Build and upload:

```sh
pio run
pio run --target upload
pio device monitor
```

`include/secrets.h`, build output, editor files, and local PlatformIO
overrides are excluded from Git.

After startup, open the IP shown on the OLED or serial console. mDNS may also
make the dashboard available at [http://plant-water.local/](http://plant-water.local/).

If Wi-Fi does not connect within 20 seconds, the controller starts:

- Network: `Plant-Watering-Setup`
- Password: `waterplant`
- Dashboard: [http://192.168.4.1/](http://192.168.4.1/)

Change `AP_PASSWORD` before deploying on an untrusted network. The fallback
access point does not provide a Wi-Fi configuration form.

## Configuration

Important values in `include/config.h`:

| Setting | Purpose |
|---|---|
| `PUMP_RUN_MS` | Maximum duration of every pump run; default is five minutes |
| `WATERING_COOLDOWN_MS` | Minimum soak time after watering |
| `SOIL_START_WATERING_PERCENT` | Moisture threshold for sensor mode |
| `SENSORLESS_FIRST_RUN_DELAY_MS` | Delay before the first fallback run |
| `SENSORLESS_WATERING_INTERVAL_MS` | Interval between fallback runs |
| `BATTERY_PUMP_CUTOFF_V` | Voltage below which pump operation is blocked |
| `TIMEZONE` | POSIX timezone used for NTP date/time |
| `PUMP_ACTIVE_HIGH` | Pump driver output polarity |
| `LED_COMMON_ANODE` | RGB LED polarity |

## Operating Modes

- **Soil sensor:** starts watering after the configured number of confirmed
  dry readings.
- **Scheduled fallback:** activates automatically when ADS1115 or soil data
  is unavailable.
- **Manual:** starts from the web dashboard while retaining duration and
  low-battery protection.

Every pump run stops after `PUMP_RUN_MS`. OLED, web, and serial output show
the active mode, run count, elapsed time, total duration, and time remaining.

Fallback scheduling is based on uptime. Its countdown and the watering run
counter restart when the controller reboots.

## Status LED

| Color | Status |
|---|---|
| Cyan | Pump running |
| Red | Battery below pump cutoff |
| Blue | Scheduled fallback active |
| Magenta | Soil sensor unavailable and fallback disabled |
| Blinking blue | Connecting to Wi-Fi |
| Yellow | Soil dry or waiting for cooldown |
| Green | Normal sensor operation |

## Calibration

### Soil

1. Record `soil raw` from serial output with the sensor dry.
2. Set that value as `SOIL_DRY_RAW`.
3. Record the value in fully wet soil.
4. Set that value as `SOIL_WET_RAW`.
5. Adjust `SOIL_START_WATERING_PERCENT`.

Either calibration value may be larger; the conversion supports both sensor
directions.

### Battery

Set:

- `BATTERY_ADC_FULL_SCALE_V` to the voltage represented by ADC reading 1023.
- `BATTERY_DIVIDER_RATIO` to `(Rtop + Rbottom) / Rbottom`.
- `BATTERY_EMPTY_V`, `BATTERY_FULL_V`, and `BATTERY_PUMP_CUTOFF_V` for the
  battery chemistry.

The percentage is a linear estimate, not a precision state-of-charge meter.
Verify reported voltage against a multimeter before relying on pump cutoff.

## OLED Troubleshooting

The serial console scans I2C at startup and checks SSD1306 addresses `0x3C`
and `0x3D`.

- No devices: verify power, common ground, SDA to D5, and SCL to D6.
- Bus stuck low: check swapped wires, shorts, and each I2C module separately.
- Address detected but blank screen: verify display dimensions and rotation.
- Six- or seven-pin modules are often SPI and need different wiring.
- Some 1.3-inch displays use SH1106 rather than SSD1306.

## Security

The dashboard has no authentication. Use it only on a trusted local network.
