// nvs_manager.h
#ifndef NVS_MANAGER_H
#define NVS_MANAGER_H

#include <Preferences.h>
#include "secrets.h"

Preferences nvsPrefs;

// Only WiFi comes from NVS — user provided via captive portal
char nvs_wifi_ssid[64] = {0};
char nvs_wifi_pass[64] = {0};

bool credentialsExist() {
  nvsPrefs.begin("credentials", true);
  bool exists = nvsPrefs.isKey("wifi_ssid");
  nvsPrefs.end();
  return exists;
}

void loadCredentials() {
  nvsPrefs.begin("credentials", true);
  nvsPrefs.getString("wifi_ssid", nvs_wifi_ssid, sizeof(nvs_wifi_ssid));
  nvsPrefs.getString("wifi_pass", nvs_wifi_pass, sizeof(nvs_wifi_pass));
  nvsPrefs.end();
}

void factoryReset() {
  nvsPrefs.begin("credentials", false);
  nvsPrefs.clear();
  nvsPrefs.end();
  Serial.println("[NVS] Factory reset, restarting...");
  delay(1000);
  ESP.restart();
}

#endif