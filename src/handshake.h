#pragma once
#include <stdint.h>
#include <stdbool.h>

// Passive WPA handshake / PMKID capture — the "feeding" half of the pwnpet.
//
// OFF by default. When armed it widens the shared WiFi promiscuous filter to
// also receive DATA frames, fishes out the EAPOL frames (the 4-way handshake +
// the PMKID in M1), and writes them to a /pwn/<ts>.pcap on the SD card for
// offline cracking (hashcat -m 22000). No deauth — strictly passive, so it only
// catches a handshake when a client genuinely (re)connects nearby.
//
// For authorized testing / your own networks / CTF only.

bool handshake_start();          // arm: power WiFi + capture DATA frames
void handshake_stop();
bool handshake_is_running();

int  handshake_pwnd_count();     // distinct APs whose EAPOL we caught this session

// Called from the WiFi promiscuous callback (WiFi task) for DATA frames while
// capture is on. Fast EAPOL check; copies a matched frame into a queue. Never
// touches the SD here.
void handshake_rx_data(const uint8_t *frame, int len, int8_t rssi, uint8_t ch);

// Drains captured frames to the session .pcap on SD. Call from loop().
void handshake_bg_tick();
