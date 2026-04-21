// rgb_led.h
#ifndef RGB_LED_H
#define RGB_LED_H

// ============================================================
//  RGB LED — common-anode, active LOW
//  Wire the common (longest) pin to 3.3 V.
//  Each colour channel is driven LOW to turn ON, HIGH to turn OFF.
//
//  State colour schema:
//    GREEN   → capturing / reading sensors
//    RED     → sensor error
//    BLUE    → transmitting MQTT
//    YELLOW  → WiFi connecting / captive-portal setup
//    WHITE   → idle / standby
// ============================================================

#define LED_R 25
#define LED_G 26
#define LED_B 27

// ── Low-level helpers ────────────────────────────────────────
// Pass true to turn a channel ON — the inversion is handled here.

inline void ledSetRaw(bool r, bool g, bool b) {
  digitalWrite(LED_R, !r);  // active LOW: HIGH = off, LOW = on
  digitalWrite(LED_G, !g);
  digitalWrite(LED_B, !b);
}

inline void ledOff() { ledSetRaw(false, false, false); }

// ── Named states ─────────────────────────────────────────────

// Green — normal steady state: connected, no errors
inline void ledOK()           { ledSetRaw(false, true,  false); }

// Red — one or more sensors returned an error (steady)
inline void ledSensorError()  { ledSetRaw(true,  false, false); }

// Yellow — WiFi connecting or captive-portal AP mode (steady)
inline void ledWifiSetup()    { ledSetRaw(true,  true,  false); }

// Yellow flashing — bad credentials, re-provisioning required
// Distinct from steady yellow so user knows action is needed
inline void ledFlashBadCredentials(int times = 6) {
  for (int i = 0; i < times; i++) {
    ledSetRaw(true, true, false);  // yellow on
    delay(200);
    ledOff();
    delay(200);
  }
}

// Blue — flash N times to signal a publish, then caller restores steady state
inline void ledFlashPublish(int times = 3) {
  for (int i = 0; i < times; i++) {
    ledSetRaw(false, false, true);  // blue on
    delay(150);
    ledOff();
    delay(100);
  }
}

// Red flashing - indicates brownout problem
inline void ledFlashBrownout(int times = 6) {
  for (int i = 0; i < times; i++) {
    ledSetRaw(true, false, false);  // red on
    delay(200);
    ledOff();
    delay(200);
  }
}

// Kept for internal use during sensor read (brief green pulse is fine
// since ledOK() is also green — user sees no change on success)
inline void ledCapturing()    { ledSetRaw(false, true,  false); }

// White — idle standby (unused in normal flow, kept for convenience)
inline void ledIdle()         { ledSetRaw(true,  true,  true);  }

// ── Init ─────────────────────────────────────────────────────

void ledInit() {
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  ledOff();  // all channels HIGH = all off on common-anode
}

#endif // RGB_LED_H