# Changelog

All notable changes to Dolos are documented here. Dates are ISO-8601.

## [0.4.0] - 2026-08-29
**Type anything, anywhere** — more keyboard layouts and a Unicode input engine.

### Added
- **10 keyboard layouts**: US / UK / DE / FR / ES / **IT / PT / SE(Nordic) / CH /
  LatAm** (`layout=…`). QWERTZ (DE/CH) and AZERTY (FR) remaps included.
- **Unicode "type anything" mode** — `STRING café`, `STRING 日本語`, or emoji now
  type correctly regardless of layout. Non-ASCII UTF-8 is injected via the target
  OS's own Unicode method, selected with `os=windows|linux|mac`:
  - Windows — Alt + numpad `+` + hex (needs `EnableHexNumpad` once)
  - Linux — Ctrl+Shift+U + hex (IBus/GTK)
  - macOS — Option + hex (needs "Unicode Hex Input")
- New DuckyScript command **`UNICODE <hex>`** (accepts `U+XXXX`).
- HID layer gained held-modifier support (`HOLD`/`KEY`/`RELEASE`) so a modifier
  can stay down across a whole hex sequence.

### Tests
- **131 host checks** (unicode 25, incl. UTF-8 decode + per-OS sequence shape).

## [0.3.0] - 2026-08-29
The **wireless console** — remote management over a secure link, with an
admin-gated remote-fire that never removes the physical-consent safeguard.

### Added — secure wireless console
- **WPA2 SoftAP** (`wifi=ap`) — never an open AP; all traffic rides the
  WPA2-encrypted link. Off by default and never started in FLASH MODE.
- **Web console** with a modern, responsive UI: status dashboard, payload
  manager (list / **view / edit / upload** — like pico-ducky), config editor
  (secrets redacted), and audit-log viewer.
- **Security core** (host-tested, 29 checks): **RBAC** (viewer / operator /
  admin), **PBKDF2-HMAC-SHA256** salted credentials, opaque **session tokens**
  (`Secure; HttpOnly; SameSite=Strict` cookies), **CSRF** tokens on every write,
  failed-login **lockout** with backoff, and **constant-time** comparisons.
- **Admin-gated remote fire**: physical BOOT arming is unchanged; remote fire
  only works while an **admin** has enabled it, and while enabled the LCD shows
  a persistent **REMOTE FIRE ARMED** banner — it can never fire covertly. The
  device also shows a WiFi indicator and, on first run, the AP SSID + a random
  admin password.

### Notes
- The console's security *logic* is unit-tested on the host; the on-device HTTP
  server + WiFi are compile-verified and pending hardware bring-up.
- Transport is WPA2 (link-layer) in this release; per-device HTTPS is next.

## [0.2.0] - 2026-08-29
A large capability release: faster injection, international layouts, a payload
picker, and professional/red-team features — still gated for **authorized** use.

### Added — injection & payloads
- **Fast injection + speed profiles.** USB HID poll interval dropped to 1 ms and
  a configurable `speed=fast|balanced|reliable` profile (~4–8× faster typing).
- **Keyboard-layout profiles** (`layout=us|uk|de|fr|es`): sends the scan codes
  that produce the right characters on international targets. US is exact;
  UK/DE/FR/ES cover the common letters, digits and symbols (verify AltGr).
- **Multi-payload SD picker**: drop several `*.txt` payloads on the card and
  choose the active one on-screen (TAP = next, HOLD = arm).

### Added — professional / enterprise
- **Dry-run preview** (`dryrun=on`): steps the payload and shows progress but
  sends **no** keystrokes — for authorized demos and verification.
- **Engagement audit log** to `/sdcard/DOLOS_AUDIT.LOG`: every run recorded with
  payload, line count, result (sent/aborted), dry-run, layout and speed.
- **On-device arm-PIN** (`armpin=…`): an optional tap-dialed code required to
  arm, so a lost device can't be misused.
- **`DOLOS.CFG` config file** on the SD card ties all of the above together.

### Added — red-team (HID envelope; no network/DoS)
- **Mouse control** in DuckyScript: `MOUSEMOVE x y`, `MOUSECLICK L|R|M`,
  `MOUSEWHEEL n` (composite keyboard+mouse device).
- **Media / consumer keys**: `MEDIA PLAY|NEXT|PREV|MUTE|VOLUP|VOLDOWN|…`.
- **Keyboard-LED exfil return channel**: the host's Caps/Num/Scroll LED state is
  captured and shown on-screen (a 3-bit channel back from the target).
- **Configurable USB identity**: set `usb_vid`/`usb_pid`/`usb_mfr`/`usb_product`
  for an authorized allow-list test. (Mechanism only — no baked-in brand
  impersonation.)

### Tests
- **77 host unit checks** (keymap 20, ducky 27, UI 7, layout 12, config 11).

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
