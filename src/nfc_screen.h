#pragma once
#include <lvgl.h>

void nfc_screen_create();
void nfc_screen_restore_power();
void nfc_screen_show();
bool nfc_screen_is_active();
void nfc_screen_worker(); // call from loop() — no-op when idle
