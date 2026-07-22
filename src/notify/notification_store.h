// notification_store.h - bounded, newest-first store of phone notifications.
//
// Both notification sources (iOS ANCS, Android/Gadgetbridge) push into this one
// store; the UI reads from it. Fixed-capacity, no heap: the newest notification
// is always index 0. Adding an id that already exists UPDATES in place and moves
// it to the front (phones re-send a notification when its content changes).
// When full, the oldest is evicted. Pure C++ so it is host-testable.
#pragma once
#include <cstdint>
#include "notification.h"

namespace notify {

// Capacity chosen to cover a realistic backlog on a glanceable device without
// spending much RAM (Capacity * sizeof(Notification) bytes, ~5 KB at 20*260).
static constexpr int kStoreCapacity = 20;

class NotificationStore {
public:
    NotificationStore() = default;

    // Insert or update. If a notification with n.uid already exists, its content
    // is overwritten and it is promoted to newest (index 0). Otherwise it is
    // inserted at the front, evicting the oldest if the store is full. The
    // epoch stamp is taken as-is from n (the caller stamps it), so this stays
    // deterministic and clock-free for tests.
    // Returns the index the notification occupies afterward (always 0).
    int add(const Notification& n);

    // Remove the notification with this uid, if present. Later entries shift up
    // to keep the array dense and order-preserving. Returns true if one was
    // removed. Honors ANCS "Removed" events and UI dismiss.
    bool remove_uid(uint32_t uid);

    // Drop everything.
    void clear();

    int  count() const { return count_; }
    bool empty() const { return count_ == 0; }
    bool full()  const { return count_ == kStoreCapacity; }

    // Newest-first access. get(0) is the most recent. Returns nullptr if out of
    // range. The pointer is valid until the next mutating call.
    const Notification* get(int index) const;

    // True if a notification with this uid is currently stored.
    bool contains(uint32_t uid) const { return index_of(uid) >= 0; }

private:
    int index_of(uint32_t uid) const;

    Notification items_[kStoreCapacity];
    int          count_ = 0;
};

}  // namespace notify
