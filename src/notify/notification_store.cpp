// notification_store.cpp - see notification_store.h.
#include "notification_store.h"
#include <cstring>

namespace notify {

int NotificationStore::index_of(uint32_t uid) const
{
    for (int i = 0; i < count_; i++) {
        if (items_[i].uid == uid) return i;
    }
    return -1;
}

int NotificationStore::add(const Notification& n)
{
    // Update-in-place path: if this uid is already stored, overwrite it and
    // promote to the front by shifting the entries above it down by one.
    int existing = index_of(n.uid);
    if (existing >= 0) {
        for (int i = existing; i > 0; i--) {
            items_[i] = items_[i - 1];
        }
        items_[0] = n;
        return 0;
    }

    // Insert at the front. If full, the last element is overwritten by the
    // shift (oldest evicted); otherwise the array grows by one.
    int last = full() ? (kStoreCapacity - 1) : count_;
    for (int i = last; i > 0; i--) {
        items_[i] = items_[i - 1];
    }
    items_[0] = n;
    if (!full()) count_++;
    return 0;
}

bool NotificationStore::remove_uid(uint32_t uid)
{
    int idx = index_of(uid);
    if (idx < 0) return false;
    for (int i = idx; i < count_ - 1; i++) {
        items_[i] = items_[i + 1];
    }
    count_--;
    return true;
}

void NotificationStore::clear()
{
    count_ = 0;
}

const Notification* NotificationStore::get(int index) const
{
    if (index < 0 || index >= count_) return nullptr;
    return &items_[index];
}

}  // namespace notify
