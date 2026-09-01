<p align="center">
  <img src="assets/banner.svg" alt="Dolos — USB-HID payload runner for the ESP32-S3-GEEK" width="100%">
</p>

<p align="center">
  <a href="https://github.com/at0m-b0mb/Dolos-ESP32-S3-GEEK/actions/workflows/host-tests.yml"><img src="https://github.com/at0m-b0mb/Dolos-ESP32-S3-GEEK/actions/workflows/host-tests.yml/badge.svg" alt="host tests"></a>
  <img src="https://img.shields.io/badge/target-ESP32--S3--GEEK-e6465a" alt="target">
  <img src="https://img.shields.io/badge/framework-ESP--IDF%20v5.5-blue" alt="esp-idf">
  <img src="https://img.shields.io/badge/license-MIT-green" alt="license">
  <img src="https://img.shields.io/badge/use-authorized%20testing%20only-important" alt="authorized use only">
</p>

<h1 align="center">Dolos</h1>

<p align="center"><b>A safety-gated USB-HID (BadUSB) payload runner with an on-screen mission control, for the Waveshare ESP32-S3-GEEK.</b><br>
<i>Built for authorized security professionals doing legal, consented testing.</i></p>

<p align="center">
  🔌 <a href="https://at0m-b0mb.github.io/Dolos-ESP32-S3-GEEK/"><b>Flash it from your browser</b></a> &nbsp;·&nbsp; no toolchain, no drivers
</p>

---

> [!WARNING]
> ## ⚖️ Ethics & Legal
> Dolos emulates a **USB keyboard**. Injecting keystrokes into a computer you do **not** own or have **explicit written authorization** to test is a crime in most jurisdictions (computer-misuse / unauthorized-access statutes such as the US CFAA, UK Computer Misuse Act, and equivalents worldwide).
>
> **Use Dolos only:** on your own equipment, in a lab you control, or under a signed engagement/scope that authorizes physical + HID testing. Keep the authorization in writing. **You are solely responsible for how you use this tool.** The authors provide it for education and authorized assessment and disclaim liability for misuse.
>
> Dolos is deliberately built to make *accidental* or *covert* firing hard — see [Safety model](#-safety-model). That is a floor, not a substitute for authorization.

---

## Why Dolos

The GEEK's USB-A plug goes straight into a host, and the ESP32-S3 has native USB — so this little board *is* a keyboard the moment you plug it in. Most BadUSB devices are **blind**: you trust that the payload ran. Dolos gives it a **1.14″ screen** and a **safety state machine**, so you always see exactly what it's doing and it never types until you decide.

|  | Dolos |
|---|---|
| **Payload language** | DuckyScript-style — `STRING`, `DELAY`, `GUI/CTRL/ALT/SHIFT` combos, F-keys, arrows, `REPEAT`, `DEFAULTDELAY`, mouse, media, `UNICODE` |
| **Payload source** | Plain-text `*.txt` on microSD (readable, auditable), picked on-screen or edited from the web console — or a harmless built-in demo |
| **On-screen mission control** | Live `SAFE → ARMED → FIRING → RUNNING → SENT` with per-line progress |
| **Arming** | Two deliberate BOOT-button holds + a 3-2-1 countdown; tap aborts anytime; optional arm-PIN |
| **Flash-mode escape hatch** | Hold BOOT while plugging in → USB-HID never starts (stays serial, safe to re-flash) |
| **Remote management** | Optional WPA2 web console with RBAC — admin-gated remote fire, never covert |
| **International** | 10 keyboard layouts + Unicode “type anything” (Win/Linux/macOS) |
| **On-device settings** | One-button menu (tap / hold / double-tap) — no touchscreen needed |
| **Payload linter** | Broken payloads are caught at load and **cannot be armed** |
| **Tested** | Pure-C engines with **225 host unit checks**, incl. a sanitized fuzz suite |

<p align="center">
  <img src="docs/img/dui_safe.png" width="49%" alt="SAFE screen">
  <img src="docs/img/dui_countdown.png" width="49%" alt="Firing countdown">
</p>
<p align="center">
  <img src="docs/img/dui_running.png" width="49%" alt="Running with live progress">
  <img src="docs/img/dui_done.png" width="49%" alt="Payload sent">
</p>
<p align="center"><sub>The on-screen UI, rendered by the actual firmware UI code (240×135).</sub></p>

## 🛡 Safety model

Dolos is engineered so it will **not** fire by accident and **cannot** fire covertly without physical interaction at the device:

1. **Boots SAFE.** It enumerates as a keyboard that sends *nothing*. The screen says so in green.
2. **Two-stage arming.** `SAFE ──hold BOOT──▶ ARMED ──hold BOOT──▶ 3·2·1 countdown ──▶ RUNNING`. A **tap aborts** at any stage, and **ARMED times out** back to SAFE on its own.
3. **Flash mode.** Hold BOOT *while plugging in* and USB-HID never initializes — the device stays a plain serial port that can never type and is trivial to re-flash.
4. **Readable payloads.** The payload is plain text on the SD card. No hidden embedded payload; with no card, a harmless demo that only types a banner runs.
5. **Always-on reminder.** `AUTHORIZED USE ONLY` never leaves the screen.

## ⌨️ DuckyScript reference

Drop a file named `PAYLOAD.TXT` in the root of a FAT-formatted microSD:

```text
REM  Comment - ignored (so is a line starting with #)
DEFAULTDELAY 40           REM  ms to wait between every command
DELAY 500                 REM  wait 500 ms once
STRING Hello, world!      REM  type literal text (case + symbols preserved)
STRINGLN done             REM  type text then press ENTER
ENTER                     REM  a named key on its own
GUI r                     REM  modifier + key  (Win/Cmd + R)
CTRL ALT DELETE           REM  fold multiple modifiers
REPEAT 3                  REM  repeat the previous command 3 more times
```

**Modifiers:** `CTRL`/`CONTROL`, `ALT`, `SHIFT`, `GUI`/`WINDOWS`/`COMMAND`.
**Named keys:** `ENTER` `ESC` `TAB` `SPACE` `BACKSPACE` `DELETE` `INSERT` `HOME` `END` `PAGEUP` `PAGEDOWN` `UP` `DOWN` `LEFT` `RIGHT` `CAPSLOCK` `MENU` `PRINTSCREEN` `F1`–`F12`.

**Mouse + media:** `MOUSEMOVE x y` · `MOUSECLICK L|R|M` · `MOUSEWHEEL n` · `MEDIA PLAY|NEXT|PREV|MUTE|VOLUP|VOLDOWN`.

**Type anything (Unicode):** set `layout=` (US/UK/DE/FR/ES/IT/PT/SE/CH/LatAm) for the target's keyboard, and `os=windows|linux|mac` so non-ASCII text just works:

```text
STRING café → ¥ 日本語 🎯     REM  types via the OS Unicode method
UNICODE 1F600                 REM  or a raw codepoint (U+1F600)
```

> Windows needs `EnableHexNumpad` set once; macOS needs the "Unicode Hex Input" source; Linux (IBus/GTK) works out of the box. Layouts beyond US are best-effort — verify against your target.

## 🎛 On-device settings (one button, no touchscreen)

The GEEK's screen is **not** a touchscreen — one 1.14″ LCD and one BOOT button. So the whole settings UI runs on three gestures:

| Gesture | On the SAFE screen | In the settings menu |
|---|---|---|
| **TAP** | next payload | next item |
| **HOLD** | arm | change this setting |
| **DOUBLE-TAP** | open settings | close settings |

Change **layout · target OS · speed · dry-run · WiFi console · remote-fire**, then **SAVE TO CARD** writes it all back to `DOLOS.CFG`. Everything except WiFi applies immediately.

### 🔒 Locking the UI

Leaving the device somewhere nobody should be tinkering with it? `ui_lock` is a **level**, not a switch:

| `ui_lock=` | Settings menu | Payload switching | Arming / firing / console |
|---|---|---|---|
| `off` *(default)* | ✅ | ✅ | ✅ |
| `on` / `menu` | 🔒 | ✅ | ✅ |
| `full` | 🔒 | 🔒 | ✅ |

At `full` the device does exactly the one job it was configured for. The screen shows a **`LOCK`** badge and says `SETTINGS LOCKED`, so a locked device reads as locked rather than broken — and the lock is **not** a menu item, because a lock you can switch off from the screen it locks isn't a lock.

## 🧪 Payload linter — it won't fire a broken payload

Every payload is parse-checked when it loads. Unknown commands, bad `DELAY`/`UNICODE` arguments, a leading `REPEAT`, over-long lines, and characters that can't be typed on your selected layout/OS are all caught. If a payload has errors the LCD shows the failing line and **`ARMING BLOCKED`** — because typing garbage into someone else's machine isn't a recoverable mistake.

## 📡 Wireless console (v0.3)

**The console is on by default.** Dolos raises a **WPA2 SoftAP** with a secure web console at `http://192.168.4.1`, and the SAFE screen shows everything you need to get in:

```
AP  Dolos-4F2A
KEY K7QM4XR2TB          <- WPA2 passphrase
admin / P4XK9WDT        <- console login
```

Those are **generated on this device at first boot** and kept in NVS — there is deliberately **no shipped default credential**, because every unit flashing this firmware would share it. The screen exists so the device can show you a unique one. Pin your own in `DOLOS.CFG` if you prefer, or `wifi=off` to disable the radio.

- **Manage remotely** — browse / **view / edit / upload** payloads, edit config, read the audit log, all from the browser (like pico-ducky, but access-controlled).
- **Secure by construction** — **RBAC** (viewer / operator / admin), **PBKDF2-HMAC-SHA256** salted credentials, opaque **session cookies** (`HttpOnly; SameSite=Strict`), **CSRF** tokens on every write, failed-login **lockout**, constant-time comparisons. Traffic rides the **WPA2-encrypted** link.
- **Admin-gated remote fire** — the physical BOOT arming you love is unchanged. Remote fire only works when an **admin enables it**, and while enabled the LCD shows a persistent **`REMOTE FIRE ARMED`** banner, so it can never fire covertly. A remote fire still runs the same abortable countdown at the device.

> Security *logic* is host-unit-tested; the on-device server + WiFi are compile-verified and pending hardware bring-up. Transport is WPA2 in v0.3; per-device HTTPS is next. See [`examples/DOLOS.CFG`](examples/DOLOS.CFG).

## 💾 Shared storage (`ATTACKMODE HID STORAGE`)

Dolos presents a composite **keyboard + mass storage** device, so payloads that expect a Ducky's drive work. What it shares is deliberately **one partition, not the card**:

| | |
|---|---|
| Partition 1 | Your payloads, audit log, boot log — **never exposed** |
| Partition 2 | The share the host sees, sized by you |

The USB sector arithmetic maps the host's sector 0 to the start of that partition, so the host **cannot address a byte outside it** — not by policy, but because there is nowhere else for the arithmetic to go.

Nothing is exposed until a payload runs `ATTACKMODE HID STORAGE`. Until then the host is told the drive is empty, so it shows nothing. While it *is* exposed the firmware stops touching the card altogether (two writers on one filesystem corrupts it), and the screen carries a **`STORAGE SHARED`** banner for as long as it lasts — handing your card to another machine is not a silent act.

Partition the card once:

```bash
diskutil partitionDisk /dev/diskN MBR FAT32 DOLOS 60% FAT32 SHARE 40%
```

Set `storage_partition=` in `DOLOS.CFG` if you want a different one. `EXFIL` writes to `LOOT.TXT` on the device's own partition.

## 🔄 Factory reset & 🔒 hardening

**Factory reset** wipes the generated credentials and saved config, restarts, and mints fresh secrets (payloads are left alone). Two authorised routes: on the device via **Settings → FACTORY RESET**, or `POST /api/factory_reset` from an **admin** console session. This is the only reversal that exists — and it is on by default.

**Hardening is opt-in and permanent.** On a stock ESP32-S3 everything is plaintext flash, so anyone holding the device can read your console credentials out with `esptool read_flash` — no reflashing needed. Flash encryption closes that; Secure Boot v2 stops attacker firmware running.

```bash
./tools/harden.sh plan     # explains everything, changes nothing
```

> ⚠️ **Secure Boot v2 cannot be disabled by anyone, with any password, ever.** eFuses are one-way. Harden a device you are deploying; never harden your development board. See [`docs/HARDENING.md`](docs/HARDENING.md).

## 🚀 Flash it

**Easiest — browser flasher:** open **<https://at0m-b0mb.github.io/Dolos-ESP32-S3-GEEK/>** in Chrome/Edge, tick the authorization box, click *Install Dolos*, pick the port. Done.

**From source:**

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py build flash          # first flash: board is still a serial device
```

Because Dolos becomes a **keyboard** after boot, its serial port disappears. To re-flash, **hold BOOT while plugging in** (FLASH MODE keeps it serial), then `idf.py flash`.

## 🧠 Architecture

```
components/ducky/      DuckyScript engine, HID keymap, 10 layouts, Unicode
                       "type anything", linter, settings model  (host-tested)
components/dconsole/   Console security core: RBAC, PBKDF2 creds,
                       sessions, CSRF, lockout                  (host-tested)
components/ui/         Canvas, font, mission-control + settings UI,
                       one-button gesture recognizer            (host-tested)
main/usb_hid.c         TinyUSB HID keyboard+mouse+consumer device
main/payload.c         SD payload loader + player (honors abort)
main/dolos_main.c      Safety state machine + BOOT control + console bridge
main/console_server.c  HTTP console endpoints (+ embedded console.html)
main/net_wifi.c        WPA2 SoftAP
main/display.c         ST7789 1.14" LCD driver
test/host/             225 unit checks — `make -C test/host`
```

The engine has **zero hardware dependencies**, so the whole keymap + parser is exercised on a laptop before it ever drives a real keyboard:

```bash
make -C test/host   # 11 suites: keymap ducky ui layout config auth unicode lint fuzz menu button
                    # the fuzz suite runs under ASan+UBSan (~12,500 malformed inputs)
```

## 🏢 Enterprise features — all shipped

Everything on the original roadmap is implemented, host-tested, and released:

| Feature | Status | How you use it |
|---|---|---|
| ⚡ **Faster injection** — 1 ms USB polling + speed profiles | ✅ v0.2 | `speed=fast\|balanced\|reliable` |
| 🌍 **Keyboard layouts** — 10 of them | ✅ v0.2 / v0.4 | `layout=us\|uk\|de\|fr\|es\|it\|pt\|se\|ch\|latam` |
| 🗂 **Multi-payload picker** — on-screen | ✅ v0.2 | Drop several `*.txt` on SD; TAP = next |
| 🖥 **Target-OS profiles** — Win / macOS / Linux | ✅ v0.4 | `os=windows\|linux\|mac` (drives Unicode input) |
| 👁 **Dry-run mode** — preview, send nothing | ✅ v0.2 | `dryrun=on` |
| 📝 **Engagement audit log** | ✅ v0.2 | `/sdcard/DOLOS_AUDIT.LOG`, also viewable in the console |
| 🔒 **On-device PIN arming** | ✅ v0.2 | `armpin=231` (tap-dialed) |
| 🖱 **Extended HID** — mouse + media | ✅ v0.2 | `MOUSEMOVE/MOUSECLICK/MOUSEWHEEL`, `MEDIA …` |
| 💡 **LED exfil return channel** | ✅ v0.2 | Host Caps/Num/Scroll shown on the LCD |
| 🆔 **Configurable USB identity** | ✅ v0.2 | `usb_vid` / `usb_pid` / `usb_mfr` / `usb_product` |
| 📡 **Secure wireless console** — RBAC, PBKDF2, CSRF | ✅ v0.3 | `wifi=ap` — see [above](#-wireless-console-v03) |
| 🌐 **Unicode "type anything"** | ✅ v0.4 | `STRING café 日本語`, `UNICODE <hex>` |
| 🎛 **On-device settings menu** | ✅ v0.5 | Double-tap BOOT; `ui_lock=menu\|full` to lock |
| 🧪 **Payload linter** | ✅ v0.5 | Automatic; blocks arming on a broken payload |
| 🔑 **Per-device credentials** | ✅ v0.5 | Generated on first boot, shown on the LCD |
| 📱 **Console join QR** | ✅ v0.6 | Settings → CONSOLE INFO; scan to join the AP |
| 🔄 **Factory reset** | ✅ v0.6 | Menu, or admin-only `POST /api/factory_reset` |
| 🔒 **Flash encryption / Secure Boot** | ✅ v0.6 | Opt-in: `tools/harden.sh` |

**Next up:** per-device **HTTPS** for the console (transport is WPA2-only today) · Secure Boot v2 + flash encryption · payload encryption at rest.

See [Issues](https://github.com/at0m-b0mb/Dolos-ESP32-S3-GEEK/issues) to propose or prioritize, and the [CHANGELOG](CHANGELOG.md) for the full history.

## 📟 Hardware

| | |
|---|---|
| **Board** | Waveshare ESP32-S3-GEEK (ESP32-S3, 16 MB flash, 2 MB PSRAM) |
| **Display** | 1.14″ ST7789, 240×135 |
| **Input** | BOOT button (the single, deliberate control) |
| **Storage** | microSD (TF) for payloads |
| **USB** | USB-A plug → native USB, enumerates as an HID keyboard |

## License

MIT © [at0m-b0mb](https://github.com/at0m-b0mb). Provided for education and **authorized** security testing. No warranty; use responsibly and legally.
