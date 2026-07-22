// spike_ancs_main.cpp - Phase 0 ANCS bench spike entry point.
//
// Compiled ONLY in the `ancs_spike` PlatformIO env (which defines ANCS_SPIKE and
// excludes the real main.cpp). It stands up nothing but Serial + BLE, auto-starts
// the ANCS consumer, and prints status, so the ANCS pipeline can be proven on the
// bench without the full UI. In every other env this file is empty.
//
//   Bench steps:
//     1. Flash:  pio run -e ancs_spike -t upload
//     2. Serial: pio device monitor            (115200)
//     3. iPhone: Settings > Bluetooth > tap "Argus Watch" > Pair
//     4. Send yourself an iMessage / any notification
//     5. Expect a "[notify] ... title=... body=..." line over serial
#ifdef ANCS_SPIKE
#include <Arduino.h>
#include "ancs.h"
#include "notify/notify_center.h"

void setup()
{
    Serial.begin(115200);
    delay(400);
    Serial.println();
    Serial.println("=== ARGUS ANCS bench spike ===");
    Serial.println("Pair 'Argus Watch' from iPhone Settings > Bluetooth,");
    Serial.println("then send yourself a notification.");
    if (!ancs::start()) {
        Serial.println("[spike] ancs::start() FAILED (WiFi up, or controller init error)");
    }
}

void loop()
{
    static uint32_t last = 0;
    if (millis() - last > 5000) {
        last = millis();
        Serial.printf("[spike] running=%d advertising=%d connected=%d stored=%d\n",
                      (int)ancs::is_running(), (int)ancs::is_advertising(),
                      (int)ancs::is_connected(), notify::center().count());
    }
    delay(10);
}
#endif  // ANCS_SPIKE
