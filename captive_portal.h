// captive_portal.h
#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>

// AP name the user will see on their phone
#define AP_SSID "CompostMonitor-Setup"
#define AP_PASS "compost123"  // default password for esp32 AP, provides WAP2 encryption on air

DNSServer    dnsServer;
WebServer    portalServer(80);
Preferences  portalPrefs;

bool credentialsSaved = false;

// ============================================================
//  HTML FORM PAGE
// ============================================================
void handlePortalRoot() {
  String page = "<!DOCTYPE html><html><head>";
  page += "<meta charset='UTF-8'>";
  page += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  page += "<title>Compost Monitor Setup</title>";
  page += "<style>";
  page += "body{font-family:Arial;text-align:center;background:#f4f4f4;padding:20px;}";
  page += "h1{color:#2e7d32;}";
  page += ".card{background:white;padding:20px;margin:20px auto;max-width:400px;";
  page += "border-radius:10px;box-shadow:0 0 10px #ccc;}";
  page += "input{width:90%;padding:10px;margin:8px 0;border:1px solid #ccc;border-radius:5px;}";
  page += "button{background:#2e7d32;color:white;padding:12px 30px;border:none;";
  page += "border-radius:5px;font-size:16px;cursor:pointer;width:95%;}";
  page += "</style></head><body>";
  page += "<h1>🌱 Compost Monitor Setup</h1>";
  page += "<p>Connect your device to your home WiFi network.</p>";
  page += "<div class='card'>";
  page += "<form method='POST' action='/save'>";
  page += "<input name='wifi_ssid' placeholder='WiFi Network Name' required><br>";
  page += "<input name='wifi_pass' placeholder='WiFi Password' type='password'><br>";
  page += "<br><button type='submit'>Save & Connect</button>";
  page += "</form></div></body></html>";

  portalServer.send(200, "text/html", page);
}

// ============================================================
//  HANDLE FORM SUBMISSION
// ============================================================
void handlePortalSave() {
  String wifi_ssid = portalServer.arg("wifi_ssid");
  String wifi_pass = portalServer.arg("wifi_pass");

  if (wifi_ssid.isEmpty()) {
    portalServer.send(400, "text/html",
      "<h2>Error: WiFi name is required. <a href='/'>Go back</a></h2>");
    return;
  }

  portalPrefs.begin("credentials", false);
  portalPrefs.putString("wifi_ssid", wifi_ssid);
  portalPrefs.putString("wifi_pass", wifi_pass);
  portalPrefs.end();

  credentialsSaved = true;

  String page = "<!DOCTYPE html><html><head>";
  page += "<meta charset='UTF-8'>";
  page += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  page += "<title>Saved!</title>";
  page += "<style>body{font-family:Arial;text-align:center;padding:40px;}";
  page += "h1{color:#2e7d32;}</style></head><body>";
  page += "<h1>✅ Connected!</h1>";
  page += "<p>Your Compost Monitor will now connect to <b>" + wifi_ssid + "</b></p>";
  page += "<p>You can close this page.</p>";
  page += "</body></html>";
  portalServer.send(200, "text/html", page);

  delay(2000);
  ESP.restart();
}

// Redirect all unknown URLs to the portal page — this is what
// makes phones automatically open the page when they connect
void handlePortalRedirect() {
  portalServer.sendHeader("Location", "http://192.168.4.1/");
  portalServer.sendHeader("Cache-Control", "no-cache");
  portalServer.send(302, "text/plain", "");
}

// ============================================================
//  START CAPTIVE PORTAL
// ============================================================
void startCaptivePortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  dnsServer.start(53, "*", WiFi.softAPIP());

  portalServer.on("/", HTTP_GET, handlePortalRoot);
  portalServer.on("/save", HTTP_POST, handlePortalSave);

  // Android captive portal detection
  portalServer.on("/generate_204",         handlePortalRedirect);
  portalServer.on("/gen_204",              handlePortalRedirect);
  // iOS / macOS captive portal detection  
  portalServer.on("/hotspot-detect.html",  handlePortalRedirect);
  portalServer.on("/library/test/success.html", handlePortalRedirect);
  // Windows captive portal detection
  portalServer.on("/ncsi.txt",             handlePortalRedirect);
  portalServer.on("/connecttest.txt",      handlePortalRedirect);
  // Fallback for everything else
  portalServer.onNotFound(handlePortalRedirect);

  portalServer.begin();

  while (!credentialsSaved) {
    dnsServer.processNextRequest();
    portalServer.handleClient();
    delay(10);
  }
}

#endif