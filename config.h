// config.h
#ifndef CONFIG_H
#define CONFIG_H

// MQTT config
#define TOPIC_TELEMETRY   "compost/compost01/telemetry"

// Timing config
#define PUBLISH_INTERVAL_MS  10UL * 1000UL                    // 10 seconds for testing
#define NTP_RESYNC_MS    24UL * 60UL * 60UL * 1000UL          // 24 hours in ms
#define SLEEP_DURATION_US    1ULL * 60ULL * 1000000ULL       // 20 minutes in microseconds

// NTP config
#define NTP_SERVER1              "pool.ntp.org"
#define NTP_SERVER2              "time.nist.gov"
#define NTP_GMT_OFFSET_SEC        10800   // UTC+3 (Turkey)
#define NTP_DAYLIGHT_OFFSET_SEC   0

#endif