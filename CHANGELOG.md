# Changelog

All notable changes to Dolos are documented here. Dates are ISO-8601.

## [0.1.0] - 2026-08-21
Initial public release — a safety-gated USB-HID payload runner for the
Waveshare ESP32-S3-GEEK, for **authorized** security testing only.

### Added
- **DuckyScript engine** (pure C): `REM`/`#`, `STRING`, `STRINGLN`, `DELAY`,
  `DEFAULTDELAY`, `REPEAT`, modifier combos (`CTRL/ALT/SHIFT/GUI` + aliases),
  and named keys (`ENTER`, `TAB`, arrows, `F1`–`F12`, …). US HID keymap.
- **TinyUSB HID keyboard** device with standard descriptors.
- **On-screen mission control** (240×135 ST7789): `SAFE → ARMED → FIRING →
  RUNNING → SENT`, USB-link indicator, live per-line progress, persistent
  `LAB USE ONLY`.
- **Safety state machine**: boots SAFE, two-stage BOOT-button arming with a
  3-2-1 countdown, tap-to-abort, ARMED timeout, and a hold-BOOT-at-power-on
  **FLASH MODE** that never starts USB-HID.
- **Payloads from microSD** (`PAYLOAD.TXT`) with a harmless built-in demo
  fallback.
- **42 host unit tests** (keymap 20, ducky 22, UI 7) run with no ESP-IDF.
- **Browser flasher** (GitHub Pages) with an authorization gate, and CI that
  builds the firmware and refreshes the flasher on every tag.

### Notes
- US keyboard layout only in this release.
- Verified on hardware to enumerate as a USB HID keyboard and stay SAFE
  (does not type) until armed by hand.
