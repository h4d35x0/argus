// ans.cpp - see ans.h. Alert Notification Service GATT server for Gadgetbridge.
#include "ans.h"
#include "notify/notify_center.h"
#include "notify/notify_log.h"

#include <Arduino.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <cstring>

namespace ans {
namespace {

// Standard Bluetooth SIG assigned numbers.
constexpr uint16_t UUID_ANS          = 0x1811;   // Alert Notification Service
constexpr uint16_t UUID_NEW_ALERT    = 0x2A46;   // write: incoming notification
constexpr uint16_t UUID_SUP_NEW_CAT  = 0x2A47;   // read: supported categories
constexpr uint16_t UUID_DIS          = 0x180A;   // Device Information Service
constexpr uint16_t UUID_FW_REV       = 0x2A26;   // Firmware Revision String

bool                s_running   = false;
volatile bool       s_connected = false;
BLEServer          *s_server    = nullptr;
uint32_t            s_uid_seq   = 1;   // synthetic ids (ANS carries no stable uid)

bool wifi_active() { return WiFi.getMode() != WIFI_MODE_NULL; }

// Map an ANS CategoryID (Bluetooth spec) to our Category vocabulary.
notify::Category map_category(uint8_t c)
{
    using C = notify::Category;
    switch (c) {
        case 1:  return C::Email;
        case 2:  return C::News;
        case 3:  return C::IncomingCall;
        case 4:  return C::MissedCall;
        case 5:  return C::Social;         // SMS/MMS
        case 6:  return C::Voicemail;
        case 7:  return C::Schedule;
        case 9:  return C::Social;         // instant message
        default: return C::Other;          // 0 simple, 8 high-priority
    }
}

// Parse a New Alert write and publish it. Format (InfiniTime / Gadgetbridge):
//   [category][count] then 0x00-separated UTF-8 strings (title, then body).
void parse_new_alert(const uint8_t *d, size_t len)
{
    if (len < 2) return;
    uint8_t cat = d[0];
    // Walk the 0x00-separated payload after the 2-byte header, skipping empties.
    const char *fields[2] = { nullptr, nullptr };
    int nf = 0;
    size_t i = 2;
    while (i < len && nf < 2) {
        while (i < len && d[i] == 0x00) i++;        // skip separators
        if (i >= len) break;
        fields[nf++] = (const char *)&d[i];
        while (i < len && d[i] != 0x00) i++;         // to next separator
        // The characteristic value is not guaranteed NUL-terminated at the end;
        // we copy with a bounded length below, so a missing final 0x00 is fine.
    }

    notify::Notification n;
    n.uid = s_uid_seq++;
    n.category = map_category(cat);

    // Copy field 0 -> title, field 1 -> body, bounded and NUL-terminated. Because
    // fields[] point into d[] which may lack a terminator on the last field, cap
    // each copy at the remaining buffer length.
    auto copy_field = [&](const char *src, char *dst, int cap) {
        if (!src) return;
        size_t max = (size_t)(&((const char *)d)[len] - src);
        size_t k = 0;
        while (k < (size_t)(cap - 1) && k < max && src[k] != 0x00) { dst[k] = src[k]; k++; }
        dst[k] = 0;
    };
    copy_field(fields[0], n.title, notify::kTitleLen);
    copy_field(fields[1], n.body,  notify::kBodyLen);
    strncpy(n.app, "Phone", notify::kAppLen - 1);

    notify::publish(n);
}

class NewAlertCb : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        std::string v = c->getValue();
        parse_new_alert((const uint8_t *)v.data(), v.size());
    }
};
NewAlertCb s_new_alert_cb;

class ServerCb : public BLEServerCallbacks {
    void onConnect(BLEServer *) override { s_connected = true; }
    void onDisconnect(BLEServer *) override {
        s_connected = false;
        if (s_running) BLEDevice::startAdvertising();   // reconnectable
    }
};
ServerCb s_server_cb;

}  // namespace

bool start()
{
    if (s_running) return true;
    if (wifi_active()) return false;

    // Advertise as an InfiniTime so Gadgetbridge auto-detects the device and
    // drives it through the standard Alert Notification Service.
    BLEDevice::init("InfiniTime");

    s_server = BLEDevice::createServer();
    if (!s_server) { BLEDevice::deinit(false); return false; }
    s_server->setCallbacks(&s_server_cb);

    // Alert Notification Service with the New Alert (write) characteristic that
    // Gadgetbridge pushes notifications to, plus the read-only supported-category
    // bitmask (advertise "all categories").
    BLEService *ans = s_server->createService(BLEUUID(UUID_ANS));
    BLECharacteristic *new_alert = ans->createCharacteristic(
        BLEUUID(UUID_NEW_ALERT),
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    new_alert->setCallbacks(&s_new_alert_cb);

    BLECharacteristic *sup = ans->createCharacteristic(
        BLEUUID(UUID_SUP_NEW_CAT), BLECharacteristic::PROPERTY_READ);
    uint8_t all_cats[2] = { 0xFF, 0x03 };   // all defined categories supported
    sup->setValue(all_cats, sizeof(all_cats));
    ans->start();

    // Minimal Device Information Service; Gadgetbridge's InfiniTime coordinator
    // reads a firmware revision during setup.
    BLEService *dis = s_server->createService(BLEUUID(UUID_DIS));
    BLECharacteristic *fw = dis->createCharacteristic(
        BLEUUID(UUID_FW_REV), BLECharacteristic::PROPERTY_READ);
    fw->setValue("1.7.0");
    dis->start();

    // Just-works bonded pairing, same posture as the HID mouse.
    BLESecurity security;
    security.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
    security.setCapability(ESP_IO_CAP_NONE);
    security.setKeySize(16);
    security.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    security.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

    BLEAdvertising *adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(BLEUUID(UUID_ANS));
    adv->setScanResponse(true);
    adv->start();

    s_running   = true;
    s_connected = false;
    NLOGLN("[ans] started - advertising as InfiniTime for Gadgetbridge");
    return true;
}

void stop()
{
    if (!s_running) return;
    s_running   = false;
    s_connected = false;
    BLEDevice::deinit(false);
    s_server = nullptr;
}

bool is_running()   { return s_running;   }
bool is_connected() { return s_connected; }

}  // namespace ans
