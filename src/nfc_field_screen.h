#pragma once
#include <lvgl.h>

// NFC reader-field detector (#5). Modal tool: while open it powers the ST25R3916
// and polls the External Field Detector (efd_o via rfalIsExtFieldOn) WITHOUT
// starting discovery - so our own field stays off and efd_o reflects an EXTERNAL
// reader's field. Alerts when an active RFID/NFC reader is within a few cm (a
// pocket-skim attempt). Passive; transmits nothing. Powers NFC down on exit.
//
// Range is near-field (centimetres) by physics - this catches close-contact
// skimming, not room-scale surveillance.

void nfc_field_screen_create();
void nfc_field_screen_show();
bool nfc_field_screen_is_active();
