#pragma once

// The "pwnpet" — a Tamagotchi-style mascot. Its mood reacts to what the rest of
// the firmware sees: meeting another Pwnagotchi over the air (the detector in
// pwnagotchi_peer), a tracker shadowing you (Threat Radar), or just an empty
// room. XP grows with friends met and is persisted to /pwn/pet.txt. While the
// screen is open it powers the WiFi scanner so peers are met live. Swipe up to
// return to Tools. (Passive WPA/PMKID capture is a planned next step.)

void pet_screen_create();
void pet_screen_show();
bool pet_screen_is_active();
