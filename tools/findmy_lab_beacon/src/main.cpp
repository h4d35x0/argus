#include <Arduino.h>
#include <BLEAdvertising.h>
#include <BLEDevice.h>
#include <BLEUtils.h>

// Controlled defensive test beacon for ARGUS tracker-detection validation.
// It advertises the Bluetooth SIG Find My Network service UUID (0xFD44) with a
// stable board address. It contains no Apple key material, cannot be claimed in
// Find My, and does not transmit location data.
static constexpr uint16_t kFindMyNetworkUuid = 0xFD44;
static constexpr uint16_t kAdvInterval = 1600;  // 1600 * 0.625 ms = 1 second

static BLEAdvertising *s_advertising = nullptr;

void setup()
{
    Serial.begin(115200);
    delay(1000);
    Serial.println();
    Serial.println("ARGUS controlled Find My lab beacon");

    BLEDevice::init("ARGUS-LAB");
    BLEDevice::setPower(ESP_PWR_LVL_N12);

    BLEAdvertisementData data;
    data.setFlags(0x06);
    data.setCompleteServices(BLEUUID(kFindMyNetworkUuid));
    data.setName("ARGUS-LAB");

    s_advertising = BLEDevice::getAdvertising();
    s_advertising->setScanResponse(false);
    s_advertising->setMinInterval(kAdvInterval);
    s_advertising->setMaxInterval(kAdvInterval);

    if (!s_advertising->setAdvertisementData(data)) {
        Serial.println("ERROR: advertisement data rejected");
        return;
    }
    if (!s_advertising->start()) {
        Serial.println("ERROR: advertising failed to start");
        return;
    }

    Serial.println("Advertising UUID 0xFD44 at -12 dBm once per second");
}

void loop()
{
    static uint32_t last_status_ms = 0;
    const uint32_t now = millis();
    if (now - last_status_ms >= 10000u) {
        last_status_ms = now;
        Serial.printf("ARGUS-LAB advertising=%d uptime=%lus\n",
                      s_advertising && s_advertising->isAdvertising(),
                      static_cast<unsigned long>(now / 1000u));
    }
    delay(20);
}
