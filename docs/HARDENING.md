# Hardening Dolos

Two very different things get called "locking down the device". This document
separates them, because one is reversible and one is permanent forever.

| | Reversible? | Protects against |
|---|---|---|
| **Factory reset** (built in) | Yes — any time, by an admin | A device changing hands: wipes credentials, config and payload state |
| **Flash encryption** | Development mode: a few times. **Release mode: never** | Reading your Wi-Fi key and admin password out of the chip |
| **Secure Boot v2** | **Never. Not by anyone. Ever** | Running attacker-supplied firmware on your device |

Dolos ships with **none of the eFuse options enabled**. They are opt-in, and
this page explains what you are giving up before you give it up.

---

## Why you would want this at all

On a stock ESP32-S3, everything Dolos stores is in **plaintext flash**. Anyone
holding the device can read your console credentials out of it in one command —
no soldering, no exploit, no reflashing:

```bash
esptool.py -p /dev/ttyUSB0 read_flash 0x9000 0x6000 nvs.bin
strings nvs.bin
```

That is the actual attack. Re-flashing the device is *not* how someone gets in,
because they never needed to. Only flash encryption closes that hole.

---

## The question everyone asks: can an admin undo it later?

**No.** There is no password, key, or tool — from Espressif or anyone else —
that disables Secure Boot v2 once burned. eFuses are one-way: bits go from 0 to
1 and never back. A "master unlock" would defeat the entire point of the
feature, so the hardware does not implement one.

The nearest thing to a reversible option is **flash encryption in Development
mode**, which can be disabled a limited number of times. But it is *not*
authenticated: anyone with physical access and esptool can disable it. It
protects against a casual reader, not against the attacker you are worried
about. It exists so you can keep developing, not as a security tier.

So the decision is genuinely one-way, and you should treat it that way:

> **Harden a device you are deploying. Never harden your development board.**

If you want "an authorized person can return this device to a clean state",
that is the **factory reset** below — which is reversible, authenticated, and
on by default.

---

## Factory reset (built in, reversible, on by default)

Wipes the generated console credentials and the saved configuration, then
restarts and mints fresh secrets. Two ways to reach it, both authorised:

* **On the device** — `double-tap BOOT` → **FACTORY RESET** → hold. Physical
  access is the authorisation, and it is blocked when `ui_lock` is set.
* **From the console** — an **admin** session only, with a CSRF token, at
  `POST /api/factory_reset`.

It does not touch the firmware, and it cannot undo flash encryption or Secure
Boot — nothing can.

---

## Flash encryption (opt-in, mostly permanent)

Encrypts the firmware **and NVS** with a key that lives in eFuse and is never
readable over the wire. This is what stops the `read_flash` attack above.

```bash
tools/harden.sh plan            # show exactly what would happen, change nothing
tools/harden.sh encrypt-dev     # development mode: still re-flashable
tools/harden.sh encrypt-release # PERMANENT. asks you to type a confirmation
```

**Development mode** keeps the UART bootloader able to re-encrypt what you
flash, so `idf.py flash` still works. Use this first, always.

**Release mode** disables that. From then on the only way to update the device
is a signed OTA image. Get this wrong and the device is finished — there is no
recovery path, no JTAG rescue, no Espressif support call.

---

## Secure Boot v2 (opt-in, permanent, no exceptions)

The bootloader will only run firmware signed with your private key.

```bash
tools/harden.sh secure-boot-key   # generate a signing key (keep it safe!)
tools/harden.sh secure-boot       # PERMANENT once flashed and booted
```

**If you lose the signing key, you can never update that device again.** Not
with a password, not with a backup, not by erasing flash. Back the key up
somewhere you would back up a production signing key, because that is what it
is.

---

## Recommended order

1. Use the device unhardened while you build payloads and learn it.
2. Run **factory reset** before handing it to anyone.
3. For a unit you will actually leave somewhere: `encrypt-dev` first, confirm
   everything still works, then `secure-boot`, then `encrypt-release` last.
4. Keep the signing key backed up offline, and label the hardened device — you
   cannot tell by looking, and you cannot go back.
