# ARGUS controlled Find My lab beacon

This standalone Seeed Studio XIAO ESP32-C6 utility advertises the Bluetooth SIG
Find My Network service UUID (`0xFD44`) for defensive validation of ARGUS's BLE
tracker-detection pipeline.

It is deliberately not an AirTag emulator. It contains no Apple company
identifier, offline-finding public key, owner identity, or location payload. It
cannot be claimed or located through Apple's Find My network. Its stable BLE
address lets the watch accumulate controlled cross-location evidence, and its
radio is limited to -12 dBm.

Build:

Run from this directory (`tools/findmy_lab_beacon`). The `pio` CLI ships with
PlatformIO and may not be on PATH.

```powershell
pio run
```

Upload:

```powershell
pio run -t upload --upload-port COMx
```

The serial monitor runs at 115200 baud. Remove power when the controlled test is
complete.
