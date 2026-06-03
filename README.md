# 🌱 Smart Compost Monitoring

An ESP32-based IoT system that monitors compost conditions and publishes telemetry to a cloud dashboard via MQTT.

---

## Overview

The device measures internal compost conditions (temperature, humidity, pressure, gas activity) alongside ambient environmental data, then sends signed telemetry every 20 minutes. A React-based web dashboard displays live readings, historical trends, battery status, and alerts.

---

## Hardware

| Component | Role |
|---|---|
| ESP32-WROOM-32 | Main MCU, Wi-Fi, HMAC signing |
| BME680 | Internal temperature, humidity, pressure, gas resistance |
| DS18B20 (waterproof) | Compost core temperature |
| DHT22 | Ambient temperature and humidity |
| 18650 Li-ion (2600 mAh) | Battery power |
| RGB LED (common-anode) | Local status indicator |
| Push button | Factory reset |

**LED status guide:**

| Color | Meaning |
|---|---|
| 🟡 Yellow (steady) | Wi-Fi connecting or captive portal active |
| 🟡 Yellow (flashing) | Bad credentials — re-provisioning needed |
| 🔵 Blue (flash) | Publishing MQTT telemetry |
| 🟢 Green | Normal operation |
| 🔴 Red (steady) | Sensor error |
| 🔴 Red (flashing) | Brownout detected |
| Off | Deep sleep |

---

## Firmware

The firmware runs a duty-cycled loop:

1. Wake from deep sleep
2. Read BME680, DHT22, DS18B20, and battery voltage
3. Validate sensor values; set `null` and error flags for invalid readings
4. Build JSON telemetry packet
5. Sign packet with HMAC-SHA256
6. Publish to MQTT broker
7. Return to deep sleep for ~20 minutes

### First-Time Setup

On first boot (or after factory reset), the device starts a captive portal:

1. Connect your phone to the `CompostMonitor-Setup` Wi-Fi network (password: `compost123`)
2. A setup page will appear automatically
3. Enter your home Wi-Fi credentials and tap **Save & Connect**
4. Credentials are stored in non-volatile memory and persist across reboots

To reset credentials, hold the physical reset button. The device will erase stored Wi-Fi credentials and return to provisioning mode.

---

## MQTT & Data Schema

**Topic:** `group6/{deviceId}/telemetry`  
**QoS:** 1

```json
{
  "ts": 1777539219,
  "ts_type": "unix",
  "nonce": 2603414739,
  "dev": "compost01",
  "data": {
    "bme_temp": 16.21,
    "bme_hum": 79.87,
    "bme_pressure": 1013.29,
    "bme_gas": 4.238,
    "dht_temp": 15.5,
    "dht_hum": 65.1,
    "ds18b20_temp": 16.125,
    "bat_v": 3.818,
    "bat_pct": 60,
    "brownout": false,
    "errors": {
      "bme": false,
      "dht": false,
      "ds18b20": false
    }
  },
  "sig": "HMAC_SHA256_SIGNATURE"
}
```

The backend verifies the HMAC-SHA256 signature and rejects packets that fail validation. Nonce and timestamp fields are used to prevent replay attacks.

---

## Project Structure

```
├── esp32-test.ino       # Main firmware sketch
├── captive_portal.h     # Wi-Fi provisioning via SoftAP captive portal
├── config.h             # Timing, MQTT topics, NTP, and brownout thresholds
├── nvs_manager.h        # Non-volatile storage for Wi-Fi credentials
├── rgb_led.h            # RGB LED helpers and named status states
└── secrets.h            # MQTT credentials and HMAC key (not in version control)
```

> **Note:** `secrets.h` is excluded from version control. Create it locally with your MQTT broker credentials and HMAC secret before building.

---

## Building & Flashing

Requires the Arduino IDE or compatible environment with ESP32 board support.

**Libraries needed:**
- `WiFi`, `WebServer`, `DNSServer` (ESP32 built-in)
- `Preferences` (ESP32 built-in)
- `Adafruit BME680`
- `DHT sensor library` (Adafruit)
- `DallasTemperature` + `OneWire`
- `PubSubClient` (MQTT)
- `mbedTLS` (HMAC — ESP32 built-in)

---

## Power Budget

| Parameter | Value |
|---|---|
| Wake interval | 20 minutes |
| Active duration | ~15 seconds |
| Active fraction | ~1.23% |
| Battery | 2600 mAh 18650 Li-ion |
| Expected runtime | 14+ days |

---

## Security Notes

- Telemetry is signed with HMAC-SHA256; the backend rejects unsigned or tampered packets
- Nonce + timestamp validation prevents replay attacks
- Captive portal provisioning is protected by physical access (factory reset requires pressing the hardware button)
- **Limitation:** The current prototype does not use MQTT over TLS. Production deployments should enable TLS and flash encryption for stored credentials

---

## Team

Enes Ağaoğlu · Ali Avcı · Efe Genç · Ali Kemal Uysal · Türker Ozan Yaylacı · Mustafa Eren Kaptan  
Galatasaray University — Computer Engineering, 2026