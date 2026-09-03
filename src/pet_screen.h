#pragma once

// The ARGUS HexHound — a watch-native cyber-recon pet (replaces the
// borrowed pwnpet goldfish). The engine lives in hexhound.cpp; this screen only
// renders it in LVGL on the 502x410 AMOLED, themed ARGUS_ACCENT (steel-blue) at
// rest and HADES_RED when a threat is present. The pet evolves through five
// stages (Egg -> Packet Pup -> Beacon Beast -> Gremlin Mode -> ARGUS
// Sentinel) by doing real recon: eating WPA handshakes (handshake.*), meeting
// Pwnagotchi peers (pwnagotchi_peer.*), and cataloguing nearby APs. XP, hunger,
// energy, and bond drive its mood; state persists to SD /HexHound/pet.txt.
//
// While the screen is open it powers the shared WiFi scanner so APs/peers are
// met live. Swipe up to return to Tools. The public API is unchanged so the
// Tools "HexHound" tile keeps working via pet_screen_show().

void pet_screen_create();
void pet_screen_show();
bool pet_screen_is_active();
