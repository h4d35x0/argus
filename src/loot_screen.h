#pragma once
#include <lvgl.h>

// Loot manager (OFFENSE mode only): lists captured engagement artifacts on the
// SD card - /pwn (*.pcap), /Wardrive (*.csv), /PingSweeps (*.txt), /Screenshots
// - with per-file sizes and per-directory totals, and lets the operator offload
// (via USB-SD mass storage) or shred them. Wiping reuses the Tier-1 shred from
// offense_wipe (overwrite-then-unlink), so operator wipes and the duress wipe
// destroy data identically. Reached from the Loot tile in the Offense grid;
// BOOT / swipe-right returns to Tools.

void loot_screen_create();
void loot_screen_show();
bool loot_screen_is_active();
