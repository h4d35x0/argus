// test_notification_store.cpp - host tests for the pure notification store.
#include "wl_test.h"
#include "notification_store.h"
#include <cstring>

using notify::Notification;
using notify::NotificationStore;
using notify::Category;

// Build a notification with a given uid and title so ordering is easy to assert.
static Notification mk(uint32_t uid, const char* title, uint32_t epoch = 0)
{
    Notification n;
    n.uid   = uid;
    n.epoch = epoch;
    n.category = Category::Social;
    std::strncpy(n.title, title, notify::kTitleLen - 1);
    return n;
}

WL_TEST(store_starts_empty)
{
    NotificationStore s;
    WL_CHECK(s.empty());
    WL_CHECK_EQ(s.count(), 0);
    WL_CHECK(s.get(0) == nullptr);
    WL_CHECK(!s.contains(1));
}

WL_TEST(store_add_is_newest_first)
{
    NotificationStore s;
    s.add(mk(1, "first"));
    s.add(mk(2, "second"));
    s.add(mk(3, "third"));
    WL_CHECK_EQ(s.count(), 3);
    WL_CHECK(std::strcmp(s.get(0)->title, "third")  == 0);
    WL_CHECK(std::strcmp(s.get(1)->title, "second") == 0);
    WL_CHECK(std::strcmp(s.get(2)->title, "first")  == 0);
}

WL_TEST(store_dup_uid_updates_and_promotes)
{
    NotificationStore s;
    s.add(mk(1, "a"));
    s.add(mk(2, "b"));
    s.add(mk(3, "c"));
    // Re-add uid 1 with new content: should update in place AND move to front,
    // without growing the count (phones re-send a notification on edit).
    s.add(mk(1, "a-edited"));
    WL_CHECK_EQ(s.count(), 3);
    WL_CHECK(std::strcmp(s.get(0)->title, "a-edited") == 0);
    WL_CHECK(std::strcmp(s.get(1)->title, "c") == 0);
    WL_CHECK(std::strcmp(s.get(2)->title, "b") == 0);
    // uid 1 must appear exactly once.
    int seen = 0;
    for (int i = 0; i < s.count(); i++) if (s.get(i)->uid == 1) seen++;
    WL_CHECK_EQ(seen, 1);
}

WL_TEST(store_evicts_oldest_when_full)
{
    NotificationStore s;
    for (uint32_t i = 0; i < notify::kStoreCapacity; i++) {
        s.add(mk(i, "x"));
    }
    WL_CHECK(s.full());
    WL_CHECK_EQ(s.count(), notify::kStoreCapacity);
    // uid 0 is the oldest and sits at the tail.
    WL_CHECK(s.contains(0));
    // One more push evicts uid 0, keeps capacity, newest is the pushed one.
    s.add(mk(9999, "newest"));
    WL_CHECK_EQ(s.count(), notify::kStoreCapacity);
    WL_CHECK(!s.contains(0));
    WL_CHECK(s.contains(9999));
    WL_CHECK(std::strcmp(s.get(0)->title, "newest") == 0);
}

WL_TEST(store_remove_uid_keeps_order_dense)
{
    NotificationStore s;
    s.add(mk(1, "a"));   // becomes index 2
    s.add(mk(2, "b"));   // index 1
    s.add(mk(3, "c"));   // index 0
    WL_CHECK(s.remove_uid(2));   // remove the middle one
    WL_CHECK_EQ(s.count(), 2);
    WL_CHECK(!s.contains(2));
    WL_CHECK(std::strcmp(s.get(0)->title, "c") == 0);
    WL_CHECK(std::strcmp(s.get(1)->title, "a") == 0);
    WL_CHECK(!s.remove_uid(1234));   // removing an absent uid is a no-op
    WL_CHECK_EQ(s.count(), 2);
}

WL_TEST(store_clear_empties)
{
    NotificationStore s;
    s.add(mk(1, "a"));
    s.add(mk(2, "b"));
    s.clear();
    WL_CHECK(s.empty());
    WL_CHECK_EQ(s.count(), 0);
    WL_CHECK(s.get(0) == nullptr);
}

WL_TEST(store_get_out_of_range_is_null)
{
    NotificationStore s;
    s.add(mk(1, "a"));
    WL_CHECK(s.get(-1) == nullptr);
    WL_CHECK(s.get(1)  == nullptr);
    WL_CHECK(s.get(0)  != nullptr);
}

WL_TEST(store_preserves_fields)
{
    NotificationStore s;
    Notification n = mk(42, "Alice");
    std::strncpy(n.app,  "com.example.chat", notify::kAppLen - 1);
    std::strncpy(n.body, "hey there", notify::kBodyLen - 1);
    n.epoch = 12345;
    n.category = Category::Email;
    s.add(n);
    const Notification* got = s.get(0);
    WL_CHECK_EQ(got->uid, 42u);
    WL_CHECK_EQ(got->epoch, 12345u);
    WL_CHECK(got->category == Category::Email);
    WL_CHECK(std::strcmp(got->app,   "com.example.chat") == 0);
    WL_CHECK(std::strcmp(got->title, "Alice") == 0);
    WL_CHECK(std::strcmp(got->body,  "hey there") == 0);
}
