// notify_center.cpp - see notify_center.h.
#include "notify_center.h"
#include "notify_log.h"
#include <Arduino.h>
#include <LilyGoLib.h>

namespace notify {

NotificationStore& center()
{
    static NotificationStore s_store;
    return s_store;
}

// Cross-task hand-off of the newest arrival for the on-screen banner. Written on
// the BLE task in publish(), read on the UI thread in take_pending(). Guarded by
// a spinlock so the struct copy can't tear across the two cores.
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_pending = false;
static Notification  s_pending_notif;

bool take_pending(Notification& out)
{
    if (!s_pending) return false;   // cheap early-out, no lock on the common path
    portENTER_CRITICAL(&s_mux);
    out = s_pending_notif;
    s_pending = false;
    portEXIT_CRITICAL(&s_mux);
    return true;
}

void publish(Notification n)
{
    // Stamp arrival time in seconds since boot. The UI shows relative age
    // ("2m ago"); absolute wall-clock is not needed and avoids depending on a
    // valid RTC. millis() wraps after ~49 days, acceptable for a glance list.
    n.epoch = (uint32_t)(millis() / 1000);

    bool is_update = center().contains(n.uid);
    center().add(n);

    // Buzz once on a genuinely new notification, not on in-place content updates
    // (a phone re-sends on edit; we do not want a second buzz for that).
    if (!is_update) {
        instance.vibrator();
    }

    // Debug-only serial mirror (NLOG compiles out unless ARGUS_NOTIFY_DEBUG is
    // set). This prints notification CONTENT, so it stays OFF in normal builds to
    // keep message titles/bodies/senders off the USB serial port.
    NLOG("[notify] uid=%lu cat=%u app=\"%s\" title=\"%s\" body=\"%s\"\n",
                  (unsigned long)n.uid, (unsigned)n.category,
                  n.app, n.title, n.body);

    // Stash for the on-screen banner (drained on the UI thread). Only for new
    // arrivals, not in-place content updates, so we don't re-pop an edit.
    if (!is_update) {
        portENTER_CRITICAL(&s_mux);
        s_pending_notif = n;
        s_pending = true;
        portEXIT_CRITICAL(&s_mux);
    }
}

void retract(uint32_t uid)
{
    if (center().remove_uid(uid)) {
        NLOG("[notify] retract uid=%lu\n", (unsigned long)uid);
    }
}

void clear_all()
{
    center().clear();
}

}  // namespace notify
