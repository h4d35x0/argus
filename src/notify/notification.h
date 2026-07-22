// notification.h - the platform-agnostic notification model.
//
// One incoming phone notification, normalized so the rest of the firmware never
// has to care whether it arrived via iOS ANCS or Android/Gadgetbridge. Both
// sources fill this struct and hand it to NotificationStore; the UI reads only
// this struct. Pure C/C++ (no Arduino, no BLE, no LVGL) so it is host-testable.
#pragma once
#include <cstdint>

namespace notify {

// Coarse category, mapped from the source's own category codes. ANCS defines its
// own CategoryID set; Gadgetbridge sends its own. We collapse both onto this so
// the UI (icon, haptic pattern, filtering) has one stable vocabulary.
enum class Category : uint8_t {
    Other = 0,
    IncomingCall,
    MissedCall,
    Voicemail,
    Social,        // messaging / social apps
    Schedule,      // calendar / reminders
    Email,
    News,
    HealthFitness,
    BusinessFinance,
    Location,
    Entertainment,
    System,        // low battery, etc.
    Count
};

// Field caps. Kept small and fixed so the store is a flat, no-heap array that
// fits comfortably in RAM. Bodies past the cap are truncated (always
// NUL-terminated). Sizes include the terminator.
static constexpr int kAppLen   = 32;   // app identifier or display name
static constexpr int kTitleLen = 64;   // sender / title
static constexpr int kBodyLen  = 160;  // message text

struct Notification {
    // Source-assigned identity. For ANCS this is the 32-bit Notification UID;
    // for Gadgetbridge it is the message handle. Used to dedup updates and to
    // honor "removed" events. 0 is a valid id, so presence is tracked by the
    // store's slot occupancy, not by uid == 0.
    uint32_t uid = 0;

    // Arrival time, seconds since boot (or RTC epoch if the caller sets it).
    // The store stamps this on add() using an injectable clock so it stays
    // host-testable; hardware passes millis()/1000 or the RTC epoch.
    uint32_t epoch = 0;

    Category category = Category::Other;

    char app[kAppLen]     = {0};
    char title[kTitleLen] = {0};
    char body[kBodyLen]   = {0};
};

}  // namespace notify
