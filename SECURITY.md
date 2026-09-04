# Security Policy

ARGUS is anti-surveillance firmware for the LILYGO T-Watch Ultra. Some of its
users rely on it in situations where being tracked has real consequences, so a
flaw in the detection logic is not only a software bug. Please report anything
you find.

## Reporting a vulnerability

**Use GitHub's private vulnerability reporting** on this repository:
*Security* -> *Report a vulnerability*. That opens a private advisory visible
only to the maintainers, so nothing is disclosed publicly while it is being
fixed.

Please do **not** open a public issue for a security problem.

Include whatever you have: what you observed, how to reproduce it, the firmware
build (the commit SHA, or the version shown in Settings), and the hardware you
saw it on. A rough report is much better than no report; you do not need a
polished write-up or a working exploit.

### What to expect

This is a small project maintained in spare time, so please read these as
intentions rather than guarantees:

- an acknowledgement that the report was received and read
- an assessment of whether it reproduces, and a rough severity
- a fix, or a written explanation of why the behaviour is intended
- credit in the release notes if you want it, or none if you prefer

If you get no response at all, please assume it was missed rather than ignored,
and feel free to nudge.

## What is in scope

Anything that causes ARGUS to be wrong in a way a user would act on:

- **Detection failures.** A tracker, evil-twin AP, skimmer or surveillance
  device that ARGUS should flag and silently does not, or a way to evade a
  detector deliberately.
- **False assurance.** A screen, string or indicator that tells the user they
  are clear, or that something is confirmed, when the underlying state does not
  support the claim. These matter as much as missed detections; a user who
  trusts a wrong "Clear" is worse off than one who trusts nothing.
- **Data exposure.** ARGUS writes detection logs to the SD card that pair
  third-party device identifiers with the wearer's GPS position. Anything that
  leaks those, retains them past their expiry, records them at higher precision
  than intended, or exposes them over USB, LoRa or BLE.
- **Mesh and radio handling.** Flaws in the Meshtastic client, the LoRa tail
  broadcast, or any parser that consumes attacker-supplied frames (BLE adverts,
  WiFi beacons, NMEA, NFC tags). Parsers reachable from the air are the highest
  value target here.
- **Memory safety** anywhere a remote or physically-proximate attacker can
  reach: buffer overruns, out-of-bounds reads, or anything crashing the watch
  from over the air.

## What is out of scope

- **Physical access to an unlocked watch.** ARGUS has no at-rest encryption and
  does not claim any. Someone holding your unlocked watch can read the SD card.
- **The offensive tooling behaving offensively.** Deauth, beacon spam, handshake
  capture and the rest are deliberate features, gated behind Offense mode. Using
  them against networks you do not own is your legal problem, not a bug.
- **Upstream and third-party issues.** Bugs in the ESP32 Arduino core, LVGL,
  RadioLib, or the vendored LilyGo libraries should go to those projects,
  though we are glad to know about them.
- **Detector false positives in dense RF environments.** Known and documented
  behaviour, not a vulnerability. A false positive that is *trivially and
  deliberately inducible by an attacker* is in scope, since that is a way to
  drown a real alert.

## A note on the detection claims

ARGUS is a hobbyist research tool, not a certified safety device, and it is
offered under the MIT License with no warranty. It can miss things. It should
be treated as one more signal, never as proof that you are or are not being
followed.

**If you believe you are being stalked or are in danger, contact your local
authorities or a domestic-violence support service.** Do not rely on this
firmware, or any consumer device, as your only safeguard.
