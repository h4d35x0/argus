#pragma once
#include <lvgl.h>

// PIN pad for the Offense unlock. Neutral steel-blue - it leaks nothing about
// what it guards. First run (no PINs set) walks through setting the unlock PIN
// then the longer shred PIN. Normal run: enter a PIN -> unlock reveals Offense;
// the shred PIN runs the duress self-destruct behind a fake "Unlocking..." decoy.
// Reached by the side-button knock (Phase B); a temporary Settings entry opens it
// for now. Swipe right to cancel.

void pin_pad_screen_create();
void pin_pad_screen_show();
bool pin_pad_screen_is_active();
