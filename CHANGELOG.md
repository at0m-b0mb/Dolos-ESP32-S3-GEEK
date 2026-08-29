# Changelog

All notable changes to Dolos are documented here. Dates are ISO-8601.

## [0.6.0] - 2026-08-29
First release verified **on real hardware**, and the bugs that found were the
point. Plus a scannable console QR, an authorised factory reset, and opt-in
device hardening.

### Fixed - found by running it on the board
- **Boot loop.** `net_wifi.c` used `ESP_ERROR_CHECK` on six calls, so a harmless
  `ESP_ERR_INVALID_STATE` from `esp_event_loop_create_default()` (the loop
  already existed) *aborted and rebooted the device*, forever. Every step now
  fails gracefully and logs why: the console is a convenience, and the device
  must come up without it.
- **Garbled, overlapping text.** `esp_lcd_panel_draw_bitmap()` is asynchronous -
  it queues a DMA transfer and returns. The v0.5 dirty-row flush staged every
  span through one buffer and refilled it while the previous transfer was still
  in flight, so the panel drew one region's pixels at another's coordinates. The
  flush now waits on a transfer-done callback before reusing the buffer.
- **Death on a normal boot while surviving FLASH MODE.** The 64 KB framebuffer
  was allocated from *internal* DMA RAM, which nothing DMAs from any more, and
  it starved Wi-Fi, TinyUSB and the HTTP server. It lives in PSRAM now.
- UI task stack raised for the settings menu; `strnlen` (POSIX, not ISO C)
  dropped from the fuzz suite, which had broken the Linux CI gate.

### Security
- **Credentials are generated with a real entropy source.** `esp_random()` is
  only a true RNG while the RF subsystem is running, and credentials are minted
  *before* the radio starts - precisely the weak window. The SAR-ADC entropy
  source is now enabled for that moment and switched off before Wi-Fi or the ADC
  driver touch the hardware.
- **Credentials are no longer rotated on a firmware change.** It protected
  nothing - NVS is plaintext flash, so anyone holding the device can read the
  key out with `esptool read_flash` without reflashing - and it silently changed
  the Wi-Fi key on every rebuild. Rotation is explicit now: **NEW CREDENTIALS**
  in the menu.
- The admin password is no longer shown permanently on the SAFE screen; it lives
  on the CONSOLE INFO screen you have to open.

### Added
- **Console join QR.** Settings -> **CONSOLE INFO** shows a scannable
  `WIFI:T:WPA;S:..;P:..;;` QR - point a phone camera at it and join, no typing.
  Credentials beside it are drawn at double size and **auto-fit**: too long for
  the column and they drop to single size and clip, never spilling off the panel
  (a geometry test caught a real 164 px overflow before it reached hardware).
  QR encoding by Project Nayuki's qrcodegen, MIT, vendored with attribution.
- **Factory reset** - the only reversal Dolos can honestly offer. Wipes the
  generated secrets and saved config, restarts, mints fresh ones; payloads are
  left alone. On the device via the menu (blocked by `ui_lock`), or over the
  console at `POST /api/factory_reset` for an **admin** session with CSRF.
- **Opt-in hardening**: `docs/HARDENING.md` and `tools/harden.sh` for flash
  encryption and Secure Boot v2. The tool never burns an eFuse itself - it
  writes an sdkconfig fragment and prints the command you type, and every
  permanent step demands a typed confirmation phrase. **Nothing here is enabled
  by default.** Documented plainly: Secure Boot v2 cannot be disabled by anyone,
  with any password, ever. eFuses are one-way. There is no unlock.
- **Boot-loop guard**: two consecutive crashes and the next boot skips the
  optional subsystems and says `SAFE BOOT - RADIO OFF` on screen. A device that
  bricks itself over a bad setting is worse than one that boots without its
  radio and tells you.
- The settings menu **scrolls** now that it has ten entries, with more-above and
  more-below markers.

### Changed
- The standing on-screen reminder is **`AUTHORIZED USE ONLY`** (was
  `LAB USE ONLY`): Dolos is used on real engagements, and what makes that use
  legitimate is written authorization, not the room you are in. Its footer
  geometry is pinned by a test so widening it can never collide with the LED
  indicators again.

## [0.5.0] - 2026-08-29
On-device settings, payload validation, hardening, and a faster engine.

### Added - on-device settings menu (no touchscreen needed)
- The GEEK has one button and no touch, so the whole settings UI runs on three
  gestures: **TAP** = next item, **HOLD** = change it, **DOUBLE-TAP** = open or
  close the menu. Change layout, target OS, speed, dry-run, WiFi console and
  remote-fire, then **SAVE TO CARD** writes `DOLOS.CFG` back.
- The gesture recognizer is a pure function of (pressed, now_ms), driven through
  real timelines by 14 host tests - including "tap then hold" and coarse 50 ms
  sampling, the two cases that break naive implementations.
- **`ui_lock` is a level**, for a device nobody should be reconfiguring at the
  button: `off` (default) / `menu` (no settings screen) / `full` (also no
  payload switching, so the device does exactly the one job it was configured
  for). `on` still means `menu`, so existing configs are unaffected. Arming,
  firing and the console work at every level. The lock is deliberately not a
  menu item, an unknown value fails *open* rather than into a stuck lock, and
  the screen says what the buttons will actually do - a `LOCK` badge and
  `SETTINGS LOCKED`, so a locked device reads as locked rather than broken.

### Added - payload linter
- Every payload is parse-checked at load. Unknown commands, bad `DELAY`/`UNICODE`
  arguments, a leading `REPEAT`, over-long lines, and characters that cannot be
  typed on the selected layout/OS are all caught.
- A payload with errors **cannot be armed** - the LCD shows the failing line and
  `ARMING BLOCKED`, and the console reports it too. Typing garbage into someone
  else's machine is not a recoverable mistake.

### Added - zero-setup wireless console
- WiFi console is now **on by default**, with **per-device credentials generated
  on first boot** and kept in NVS: a 10-character WPA2 key and an 8-character
  admin password, shown on the SAFE screen. There is deliberately **no shipped
  default credential** - the screen exists so the device can show a unique one.
- `DOLOS.CFG` still wins if you want to pin your own.

### Changed - performance
- **ASCII -> HID is a 128-byte lookup** instead of a ~40-branch chain that ran
  for every character of every `STRING` (~20 comparisons per char at 8000
  chars/s on the fast profile).
- **Layout overrides are expanded once** into a direct index instead of being
  scanned linearly per character (AZERTY was ~10 extra comparisons each).
- **The display sends only rows that changed.** The old flush byte-swapped all
  32,400 pixels and pushed 64 KB over SPI every tick - about 13 ms of bus time
  per frame - even on the SAFE screen where nothing moves. An unchanged frame now
  costs one memcmp per row and no transfer at all.
- **Canvas fills clip once and write whole rows** instead of a bounds-checked
  call per pixel; out-of-bounds pixels are still counted, so the UI overflow
  guard keeps its exact meaning.

### Added - robustness
- A **sanitized fuzz suite** (ASan + UBSan) throws ~12,500 malformed inputs at
  the config parser, DuckyScript parser, linter and UTF-8 decoder: random bytes,
  truncated multi-byte sequences, over-long lines, empty and degenerate values.
- `config_defaults()` now zeroes the whole struct, so no uninitialised stack byte
  can reach a log or the console.

### Tests
- **225 host checks** (was 131): +14 button, +31 menu, +19 fuzz, +17 lint, +13 UI/config.

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
