#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <AsyncMqttClient.h>
#include <ArduinoJson.h>
#include "mbedtls/md.h"
#include <esp_sntp.h>  // NTP / SNTP for ESP32

#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include "DHT.h"
#include <OneWire.h>
#include <DallasTemperature.h>

// All system configurations exist in the files below
#include "secrets.h"
#include "config.h"
#include "nvs_manager.h"
#include "captive_portal.h"
#include "rgb_led.h"

#define DEBUG false
#define LOGGING true
#define LOG(msg) \
  if (LOGGING) Serial.println(msg)

// ============================================================
//  SENSOR PIN DEFINITIONS
// ============================================================
#define BME_SDA 22
#define BME_SCL 23
#define DHTPIN 4
#define DHTTYPE DHT22
#define ONE_WIRE_BUS 15
#define BATTERY_PIN 35
#define RESET_BUTTON_PIN 32

// ============================================================
//  OBJECTS
// ============================================================
AsyncMqttClient mqtt;

// RTC memory - survives deep sleep
// pubackReceived: set to true in the PUBACK callback before sleeping,
// so we know QoS 1 delivery was confirmed by the broker.
RTC_DATA_ATTR bool pubackReceived = false;

#if DEBUG
WebServer server(80);
#endif

Adafruit_BME680 bme;
DHT dht(DHTPIN, DHTTYPE);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);

// ============================================================
//  SENSOR VARIABLES
// ============================================================
float bmeTemp, bmeHum, bmePressure, bmeGas;
float dhtTemp, dhtHum;
float ds18b20Temp;
float batteryVoltage = 0.0;
int batteryPercent = 0;
bool brownoutPending = false;  // set by handleBrownoutIfNeeded(), consumed by publishTelemetry()

bool bmeStatus = false;
bool bmeError = false;
bool dhtError = false;
bool dsError = false;

unsigned long lastUpdate = 0;
unsigned long lastPublish = 0;

// RTC memory survives deep sleep - used to track NTP sync across wake cycles
RTC_DATA_ATTR time_t lastNtpSyncUnix = 0;  // unix timestamp of last successful sync
RTC_DATA_ATTR bool ntpSynced = false;       // true once we have at least one valid sync

// millis()-based alias kept for dashboard display only (resets each boot, that's fine)
unsigned long lastNtpSync = 0;

// ============================================================
//  NTP HELPERS
// ============================================================

// Returns true only after at least one successful NTP sync AND
// the resulting time looks plausible (year >= 2024).
bool timeIsSynced() {
  if (!ntpSynced) return false;
  struct tm ti;
  if (!getLocalTime(&ti, 0)) return false;  // 0 ms timeout - non-blocking
  return (ti.tm_year + 1900 >= 2024);
}

// Kick off an NTP sync.  configTime() is non-blocking; the ESP32 SNTP
// stack updates the system clock in the background via a callback.
void syncNTP() {
  LOG("[NTP] Requesting time sync...");
  configTime(NTP_GMT_OFFSET_SEC, NTP_DAYLIGHT_OFFSET_SEC,
             NTP_SERVER1, NTP_SERVER2);

  struct tm ti;
  if (getLocalTime(&ti, 10000) && (ti.tm_year + 1900 >= 2024)) {
    ntpSynced = true;
    lastNtpSyncUnix = time(nullptr);  // store in RTC memory - survives deep sleep
    lastNtpSync = millis();           // for dashboard display only
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
    LOG("[NTP] Time synced: " + String(buf));
  } else {
    LOG("[NTP] Sync failed - will retry next cycle");
  }
}

// Returns the current UNIX timestamp (seconds since epoch).
// Safe to call at any time; returns 0 when the clock is not yet set.
time_t getUnixTime() {
  if (!timeIsSynced()) return 0;
  return time(nullptr);  // time() calls are safe once SNTP has set the clock
}

#if DEBUG
// Fills buf with a human-readable local-time string, e.g.
// "2026-04-13 15:42:07".  Returns false and leaves buf untouched
// if the clock is not yet valid.
bool getTimeString(char* buf, size_t len) {
  if (!timeIsSynced()) return false;
  struct tm ti;
  if (!getLocalTime(&ti, 0)) return false;
  strftime(buf, len, "%Y-%m-%d %H:%M:%S", &ti);
  return true;
}
#endif

// ============================================================
//  HMAC-SHA256
// ============================================================
void computeHMAC(const char* message, char* outHex) {
  byte hmacResult[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_type_t mdType = MBEDTLS_MD_SHA256;

  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(mdType), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)HMAC_SECRET, strlen(HMAC_SECRET));
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)message, strlen(message));
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);

  for (int i = 0; i < 32; i++) {
    sprintf(outHex + (i * 2), "%02x", hmacResult[i]);
  }
  outHex[64] = '\0';
}

// ============================================================
//  PUBLISH TELEMETRY  (QoS 1 - broker must acknowledge)
// ============================================================
void publishTelemetry() {
  StaticJsonDocument<384> dataDoc;

  if (bmeError) {
    dataDoc["bme_temp"] = nullptr;
    dataDoc["bme_hum"] = nullptr;
    dataDoc["bme_pressure"] = nullptr;
    dataDoc["bme_gas"] = nullptr;
  } else {
    dataDoc["bme_temp"] = bmeTemp;
    dataDoc["bme_hum"] = bmeHum;
    dataDoc["bme_pressure"] = bmePressure;
    dataDoc["bme_gas"] = bmeGas;
  }

  if (dhtError) {
    dataDoc["dht_temp"] = nullptr;
    dataDoc["dht_hum"] = nullptr;
  } else {
    dataDoc["dht_temp"] = dhtTemp;
    dataDoc["dht_hum"] = dhtHum;
  }

  if (dsError) {
    dataDoc["ds18b20_temp"] = nullptr;
  } else {
    dataDoc["ds18b20_temp"] = ds18b20Temp;
  }

  dataDoc["bat_v"]   = batteryVoltage;
  dataDoc["bat_pct"] = batteryPercent;

  // Only present when the device is about to hibernate due to low battery.
  // Absent in all normal packets — backend should suppress offline alerts
  // for 24 hours and trigger a low-battery alert when this is true.
  if (brownoutPending) dataDoc["brownout"] = true;

  JsonObject errors = dataDoc.createNestedObject("errors");
  errors["bme"] = bmeError;
  errors["dht"] = dhtError;
  errors["ds18b20"] = dsError;

  char dataJson[384];
  serializeJson(dataDoc, dataJson, sizeof(dataJson));

  time_t unixNow = getUnixTime();
  bool hasEpoch = (unixNow > 0);
  uint32_t nonce = esp_random();

  char signable[512];
  if (hasEpoch) {
    snprintf(signable, sizeof(signable),
             "%lu|%u|%s|%s", (unsigned long)unixNow, nonce, DEVICE_ID, dataJson);
  } else {
    snprintf(signable, sizeof(signable),
             "%lu|%u|%s|%s", millis(), nonce, DEVICE_ID, dataJson);
  }

  char sig[65];
  computeHMAC(signable, sig);

  StaticJsonDocument<768> envelope;
  if (hasEpoch) {
    envelope["ts"] = (unsigned long)unixNow;
    envelope["ts_type"] = "unix";
  } else {
    envelope["ts"] = millis();
    envelope["ts_type"] = "millis";
  }
  envelope["nonce"] = nonce;
  envelope["dev"] = DEVICE_ID;
  envelope["data"] = dataDoc;
  envelope["sig"] = sig;

  char payload[768];
  serializeJson(envelope, payload, sizeof(payload));

  // Publish at QoS 1 - broker will send a PUBACK when it has received the message.
  // The onPublish callback sets pubackReceived = true when the ack arrives.
  pubackReceived = false;
  uint16_t packetId = mqtt.publish(TOPIC_TELEMETRY, 1, false, payload);

  if (packetId == 0) {
    LOG("[MQTT] Publish failed (queue full or not connected)");
  } else {
    LOG("[MQTT] Publish queued, packetId=" + String(packetId) + ", waiting for PUBACK...");
    // Wait up to 5 s for the broker to acknowledge
    unsigned long ackDeadline = millis() + 5000;
    while (!pubackReceived && millis() < ackDeadline) {
      delay(10);
    }
    if (pubackReceived) {
      LOG("[MQTT] PUBACK received - delivery confirmed");
      LOG(payload);
    } else {
      LOG("[MQTT] PUBACK timeout - packet may be lost");
    }
  }

  // Flash blue to signal transmission attempt, then restore steady state
  ledFlashPublish(3);
  bool anySensorError = (bmeError || dhtError || dsError);
  if (anySensorError) {
    ledSensorError();
  } else {
    ledOK();
  }
}

// ============================================================
//  ASYNC MQTT CALLBACKS
// ============================================================
void onMqttConnect(bool sessionPresent) {
  LOG("[MQTT] Connected (sessionPresent=" + String(sessionPresent) + ")");
  ledOK();
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  LOG("[MQTT] Disconnected, reason=" + String((int)reason));
}

void onMqttPublish(uint16_t packetId) {
  // Called when the broker sends PUBACK for a QoS 1 message
  LOG("[MQTT] PUBACK for packetId=" + String(packetId));
  pubackReceived = true;
}

// ============================================================
//  MQTT CONNECT - blocks until connected (with timeout reboot)
// ============================================================
void mqttReconnect() {
  ledWifiSetup();  // YELLOW - connecting
  LOG("[MQTT] Connecting...");
  mqtt.connect();
  unsigned long start = millis();
  while (!mqtt.connected()) {
    if (millis() - start > 10000) {
      LOG("[MQTT] Connect timeout - rebooting...");
      ESP.restart();
    }
    delay(100);
  }
}

// ============================================================
//  BATTERY
// ============================================================
float readBatteryVoltage() {
  long sum = 0;
  for (int i = 0; i < 20; i++) {
    sum += analogRead(BATTERY_PIN);
    delay(5);
  }
  float raw = sum / 20.0;
  float adcVoltage = (raw / 4095.0) * 3.3;
  float battV = adcVoltage * 2.0;
  battV *= 1.08;
  return battV;
}

int batteryPercentage(float v) {
  // Standard 18650 Li-ion: 4.20V = 100%, 3.00V = 0%
  // Lookup table based on a typical discharge curve
  if (v >= 4.20) return 100;
  if (v >= 4.10) return 90;
  if (v >= 4.00) return 80;
  if (v >= 3.90) return 70;
  if (v >= 3.80) return 60;
  if (v >= 3.70) return 50;
  if (v >= 3.60) return 40;
  if (v >= 3.50) return 30;
  if (v >= 3.40) return 20;
  if (v >= 3.20) return 10;
  if (v >= 3.00) return 5;
  return 0;  // at or below 3.00V — protect the cell, stop discharging
}

// ============================================================
//  BROWNOUT PROTECTION
//  Called after readBatteryVoltage(). Confirms low voltage with
//  5 re-samples over 500 ms to reject transient ADC glitches.
//  On confirmation, sets brownoutPending = true so the next
//  publishTelemetry() call includes brownout:true inside the
//  standard signed envelope, then hibernates for 1 day.
// ============================================================
void handleBrownoutIfNeeded() {
  if (batteryVoltage >= BROWNOUT_VOLTAGE) return;

  LOG("[Brownout] Low voltage detected (" + String(batteryVoltage, 2) + "V) — confirming...");
  int confirmCount = 0;
  for (int i = 0; i < 5; i++) {
    delay(100);
    if (readBatteryVoltage() < BROWNOUT_VOLTAGE) confirmCount++;
  }

  if (confirmCount < 4) {  // require 4 out of 5 — rejects single-sample glitches
    LOG("[Brownout] False alarm (" + String(confirmCount) + "/5 confirmed) — continuing");
    return;
  }

  LOG("[Brownout] Confirmed (" + String(confirmCount) + "/5) — flagging for hibernation");
  brownoutPending = true;
  // publishTelemetry() will be called by the normal flow in loop() and will
  // include brownout:true in the signed data object. After it returns,
  // loop() checks brownoutPending and enters hibernation.
}

// ============================================================
//  READ SENSORS
// ============================================================
void readSensors() {
  ledCapturing();  // GREEN - actively reading sensors

  brownoutPending = false;  // reset each cycle — set again by handleBrownoutIfNeeded() if needed
  batteryVoltage = readBatteryVoltage();
  batteryPercent = batteryPercentage(batteryVoltage);

  handleBrownoutIfNeeded();

  // BME680
  Wire.beginTransmission(0x77);
  if (Wire.endTransmission() != 0) {
    LOG("BME680 not found!");
    bmeStatus = false;
    bmeError = true;
  } else {
    if (!bmeStatus) {
      LOG("BME680 reconnected, reinitializing...");
      bme.begin();
      bme.setTemperatureOversampling(BME680_OS_8X);
      bme.setHumidityOversampling(BME680_OS_2X);
      bme.setPressureOversampling(BME680_OS_4X);
      bme.setGasHeater(320, 150);
      bmeStatus = true;
    }
    if (!bme.performReading()) {
      LOG("BME680 read failed!");
      bmeError = true;
      bmeTemp = NAN;
      bmeHum = NAN;
      bmePressure = NAN;
      bmeGas = NAN;
    } else {
      bmeError = false;
      // Calibrated against DS18B20 (temp) and DHT22 (humidity)
      // Dataset: 723 readings, 704 clean temp samples, 718 clean DHT samples
      // BME680 raw temp runs +0.87C hot vs DS18B20 reference (was -0.70)
      bmeTemp = bme.temperature - 0.87;
      // BME680 humidity: data-derived linear regression against DHT22
      // Old: *1.23 + 2.85, mean error 4.4%
      // New: *1.03 + 7.03, mean error 0.9% (80% improvement)
      bmeHum = bme.humidity * 1.03 + 7.03;
      bmePressure = bme.pressure / 100.0;
      bmeGas = bme.gas_resistance / 1000.0;

      // DHT22 - reads 0.17C cold vs DS18B20 (was uncalibrated)
      dhtTemp = dht.readTemperature() + 0.17;
      dhtHum  = dht.readHumidity();   // DHT22 humidity used as reference, no cal needed
    }
  }

  // DHT22
  dhtTemp = dht.readTemperature();
  dhtHum = dht.readHumidity();
  dhtError = (isnan(dhtTemp) || isnan(dhtHum) || dhtTemp < -40 || dhtTemp > 80 || dhtHum < 0 || dhtHum > 100);

  // DS18B20
  ds18b20.requestTemperatures();
  ds18b20Temp = ds18b20.getTempCByIndex(0);
  dsError = (ds18b20Temp < -55 || ds18b20Temp > 125);

  lastUpdate = millis();

  // RED steady if any sensor failed, GREEN steady otherwise
  bool anySensorError = (bmeError || dhtError || dsError);
  if (anySensorError) {
    ledSensorError();  // RED - sensor fault
  } else {
    ledOK();           // GREEN - all sensors healthy
  }
}

// ============================================================
//  LOCAL WEB DASHBOARD - debug mode only
// ============================================================
#if DEBUG
void handleRoot() {
  String page = "<!DOCTYPE html><html><head>";
  page += "<meta charset='UTF-8'>";
  page += "<meta http-equiv='refresh' content='5'>";
  page += "<title>Smart Compost Monitor</title>";
  page += "<style>";
  page += "body{font-family:Arial;text-align:center;background:#f4f4f4;}";
  page += "h1{color:#2e7d32;}";
  page += ".card{background:white;padding:20px;margin:20px;border-radius:10px;box-shadow:0 0 10px #ccc;}";
  page += ".error{color:red;font-weight:bold;}";
  page += ".mqtt_ok{color:green;font-weight:bold;}";
  page += ".mqtt_err{color:orange;font-weight:bold;}";
  page += ".ntp_ok{color:green;}";
  page += ".ntp_err{color:orange;}";
  page += "</style></head><body>";

  page += "<h1>🌱 Smart Compost Monitor</h1>";

  // ---- Current time / NTP status ----
  char timeBuf[32];
  if (getTimeString(timeBuf, sizeof(timeBuf))) {
    page += "<p>🕐 <b>" + String(timeBuf) + "</b> (UTC+3)</p>";
    unsigned long syncAge = (millis() - lastNtpSync) / 1000UL;
    page += "<p class='ntp_ok'>● NTP synced &mdash; last sync ";
    page += String(syncAge);
    page += " s ago</p>";
  } else {
    page += "<p class='ntp_err'>● NTP not yet synced - uptime: ";
    page += String(millis() / 1000);
    page += " s</p>";
  }

  // ---- Last sensor update ----
  page += "<p>Last sensor update: " + String(lastUpdate / 1000) + " s since start</p>";

  // ---- MQTT status ----
  if (mqtt.connected()) {
    page += "<p class='mqtt_ok'>● MQTT connected</p>";
  } else {
    page += "<p class='mqtt_err'>● MQTT disconnected</p>";
  }

  if (bmeError || dhtError || dsError) {
    page += "<h2 class='error'>⚠ Sensor error detected!</h2>";
    if (bmeError) page += "<p class='error'>BME680 reading abnormal!</p>";
    if (dhtError) page += "<p class='error'>DHT22 reading abnormal!</p>";
    if (dsError) page += "<p class='error'>DS18B20 reading abnormal!</p>";
  }

  page += "<div class='card'><h2>Battery</h2>";
  page += "Loaded Voltage: " + String(batteryVoltage, 2) + " V<br>";
  page += "Estimated Charge: " + String(batteryPercent) + " %</div>";

  page += "<div class='card'><h2>DS18B20</h2>";
  page += "Temperature: " + String(ds18b20Temp) + " °C</div>";

  page += "<div class='card'><h2>BME680</h2>";
  page += "Temp: " + String(bmeTemp) + " °C<br>";
  page += "Humidity: " + String(bmeHum) + " %<br>";
  page += "Gas Resistance: " + String(bmeGas) + " kΩ<br>";
  page += "Pressure: " + String(bmePressure) + " hPa</div>";

  page += "<div class='card'><h2>DHT22</h2>";
  page += "Temp: " + String(dhtTemp) + " °C<br>";
  page += "Humidity: " + String(dhtHum) + " %</div>";

  page += "</body></html>";
  server.send(200, "text/html", page);
}
#endif

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  LOG("Starting Smart Compost Monitor");

  ledInit();
  ledWifiSetup();  // YELLOW - provisioning / WiFi not yet connected

  // ── Factory reset via long press ─────────────────────────────
  // Hold the button for 5 seconds on boot to wipe credentials
  // and re-enter the captive portal (e.g. for network change).
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    LOG("[Reset] Button held — hold 5s to factory reset...");
    unsigned long holdStart = millis();
    while (digitalRead(RESET_BUTTON_PIN) == LOW) {
      // Flash red while waiting to show progress
      ledSensorError();
      delay(200);
      ledOff();
      delay(200);
      if (millis() - holdStart >= 5000) {
        LOG("[Reset] 5s reached — wiping credentials...");
        // Flash red rapidly to confirm reset before wiping
        for (int i = 0; i < 10; i++) {
          ledSensorError();
          delay(100);
          ledOff();
          delay(100);
        }
        factoryReset();  // wipes NVS and reboots into captive portal
      }
    }
    // Released before 5s — ignore and continue normal boot
    LOG("[Reset] Released early — continuing normal boot");
    ledWifiSetup();
  }

  if (!credentialsExist()) {
    startCaptivePortal();
  }

  loadCredentials();

  dht.begin();
  ds18b20.begin();
  analogReadResolution(12);

  Wire.begin(BME_SDA, BME_SCL);
  if (!bme.begin()) {
    LOG("BME680 not found!");
    while (1)
      ;
  }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setGasHeater(320, 150);
  bmeStatus = true;

  WiFi.persistent(false);  // don't write WiFi credentials to flash every boot
  WiFi.mode(WIFI_STA);     // ensure clean STA mode after deep sleep

  WiFi.begin(nvs_wifi_ssid, nvs_wifi_pass);
  Serial.print("Connecting to WiFi");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    wl_status_t status = WiFi.status();

    // Wrong password or SSID not found — only triggered during initial setup
    // For normal outages the device will just timeout and reboot to retry
    if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
      LOG("\n[WiFi] Connection rejected (status=" + String(status) + ") — rebooting to retry...");
      ESP.restart();
    }

    if (millis() - wifiStart > 20000) {
      LOG("\n[WiFi] Timeout — rebooting to retry...");
      ESP.restart();
    }
    delay(500);
    Serial.print(".");
  }

  LOG("\nWiFi connected!");
  LOG("IP: " + WiFi.localIP().toString());
  ledOK();

  // Only sync NTP if we haven't yet, or if 24 h have passed since last sync
  if (!ntpSynced || (time(nullptr) - lastNtpSyncUnix >= NTP_RESYNC_MS / 1000UL)) {
    syncNTP();
  } else {
    // Clock is already valid from RTC - restore it so getLocalTime() works
    configTime(NTP_GMT_OFFSET_SEC, NTP_DAYLIGHT_OFFSET_SEC, NTP_SERVER1, NTP_SERVER2);
    LOG("[NTP] Skipping sync - last sync was " + String(time(nullptr) - lastNtpSyncUnix) + "s ago");
  }

  mqtt.onConnect(onMqttConnect);
  mqtt.onDisconnect(onMqttDisconnect);
  mqtt.onPublish(onMqttPublish);
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCredentials(MQTT_USER, MQTT_PASS);
  String clientId = String("compost-") + String(DEVICE_ID) + "-" + String(random(0xffff), HEX);
  mqtt.setClientId(clientId.c_str());
  mqttReconnect();

#if DEBUG
  server.on("/", handleRoot);
  server.begin();
  LOG("[Web] Dashboard available at http://" + WiFi.localIP().toString());
#endif
}

// ============================================================
//  LOOP
//  DEBUG true  → test mode: publish every 10 s, dashboard always on
//  DEBUG false → production: read once, publish once, deep sleep 20 min
// ============================================================
void loop() {
#if DEBUG
  // ── TEST MODE ───────────────────────────────────────────────
  if (!mqtt.connected()) mqttReconnect();

  // Read sensors on the same interval as publish so the LED
  // state is visible between cycles instead of flickering
  if (millis() - lastPublish >= PUBLISH_INTERVAL_MS) {
    readSensors();
    publishTelemetry();
    lastPublish = millis();

    if (brownoutPending) {
      ledFlashBrownout(6);  // RED flash — signals critical battery before going dark
      LOG("[Brownout] Entering hibernation...");
      mqtt.disconnect();
      delay(200);
      WiFi.disconnect(true);
      delay(100);
      ledOff();
      esp_deep_sleep(BROWNOUT_SLEEP_US);  // 1 day — retry after cell recovers slightly
    }
  }

  server.handleClient();

#else
  // ── PRODUCTION MODE ─────────────────────────────────────────
  if (!mqtt.connected()) mqttReconnect();

  readSensors();
  publishTelemetry();

  if (brownoutPending) {
    ledFlashBrownout(6);  // RED flash — signals critical battery before going dark
    LOG("[Brownout] Entering hibernation...");
    mqtt.disconnect();
    delay(200);
    WiFi.disconnect(true);
    delay(100);
    ledOff();
    esp_deep_sleep(BROWNOUT_SLEEP_US);  // 1 day — retry after cell recovers slightly
  }

  // If a sensor error occurred, hold the red LED visible for 2 s
  // before sleeping so the user can actually see it
  if (bmeError || dhtError || dsError) {
    ledSensorError();
    delay(2000);
  }

  LOG("[Sleep] Entering deep sleep...");
  mqtt.disconnect();
  delay(200);  // let the DISCONNECT packet send before cutting power
  WiFi.disconnect(true);
  delay(100);
  ledOff();
  esp_deep_sleep(SLEEP_DURATION_US);
#endif
}
