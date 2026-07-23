#pragma once
#include <stdint.h>
#include <stdbool.h>

// Probe-request sniffer (OFFENSE recon). Passively logs nearby devices from the
// 802.11 probe requests they broadcast, and - when a device sends a DIRECTED
// probe - the network name it is looking for. That name is a network the device
// has connected to before, so the list quietly reveals where a device has been
// (home/work SSIDs) and which devices are around you. Never transmits.
//
// Brings WiFi up itself (guarded against BLE); mutually exclusive with the WiFi
// injection tools (single radio). Offense-gated.

bool probe_sniffer_start();   // false if WiFi can't be brought up / another tool holds it
void probe_sniffer_stop();
bool probe_sniffer_is_running();

void probe_sniffer_screen_create();
void probe_sniffer_screen_show();
bool probe_sniffer_screen_is_active();
