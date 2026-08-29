#!/usr/bin/env bash
#
# harden.sh - opt-in hardening for a Dolos device.
#
# This script never burns an eFuse itself. It writes an sdkconfig fragment and
# tells you the exact command that makes the change permanent, so the
# irreversible step is always something you type deliberately, on a device you
# chose, and not a side effect of running a helper.
#
# See docs/HARDENING.md. The short version: none of this can be undone. There
# is no password that reverts Secure Boot. Do not do this to your dev board.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FRAG="$ROOT/sdkconfig.hardening"
KEY="$ROOT/secure_boot_signing_key.pem"

c_red()  { printf '\033[31m%s\033[0m\n' "$*"; }
c_grn()  { printf '\033[32m%s\033[0m\n' "$*"; }
c_ylw()  { printf '\033[33m%s\033[0m\n' "$*"; }
c_bold() { printf '\033[1m%s\033[0m\n' "$*"; }

require_phrase() {
    local phrase="$1"
    c_red "This CANNOT be undone. Not by you, not by an admin password, not by"
    c_red "erasing the flash. If it goes wrong the device is scrap."
    echo
    echo "Type exactly:  $phrase"
    printf '> '
    read -r typed
    if [ "$typed" != "$phrase" ]; then
        c_ylw "Did not match. Nothing was changed."
        exit 1
    fi
}

append_frag() {
    touch "$FRAG"
    for line in "$@"; do
        grep -qxF "$line" "$FRAG" || echo "$line" >> "$FRAG"
    done
    c_grn "wrote $FRAG"
    echo
    c_bold "Apply it with:"
    echo "  cd $ROOT"
    echo "  SDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.hardening' idf.py fullclean build"
    echo "  idf.py -p <PORT> flash        # the eFuse burn happens on first boot"
}

cmd_plan() {
    c_bold "Dolos hardening - plan only, nothing will change"
    echo
    echo "Current state of this checkout:"
    if [ -f "$FRAG" ]; then
        c_ylw "  sdkconfig.hardening EXISTS:"
        sed 's/^/    /' "$FRAG"
    else
        echo "  sdkconfig.hardening: absent (device builds unhardened)"
    fi
    [ -f "$KEY" ] && c_ylw "  signing key EXISTS: $KEY" \
                  || echo "  signing key: absent"
    echo
    c_bold "What each option does"
    echo "  encrypt-dev      encrypt firmware + NVS; re-flashing still works."
    echo "                   Stops 'esptool read_flash' recovering your"
    echo "                   Wi-Fi key and admin password. START HERE."
    echo "  secure-boot-key  generate the signing key (do this before secure-boot)"
    echo "  secure-boot      only your signed firmware will boot. PERMANENT."
    echo "  encrypt-release  UART re-flashing disabled for good. PERMANENT."
    echo
    c_red "None of the permanent options can be reverted by anyone, with any"
    c_red "password. For 'an admin can reset this device', use FACTORY RESET"
    c_red "in the on-device menu or POST /api/factory_reset - that IS reversible."
}

cmd_encrypt_dev() {
    c_bold "Flash encryption - DEVELOPMENT mode"
    echo "Firmware and NVS are encrypted with a key held in eFuse. The UART"
    echo "bootloader can still re-encrypt what you flash, so 'idf.py flash'"
    echo "keeps working. This is the reversible-ish tier and the right first step."
    echo
    c_ylw "Note: it can be disabled only a limited number of times, and doing so"
    c_ylw "needs no password - it protects against a reader, not a determined"
    c_ylw "attacker with the device in hand."
    echo
    require_phrase "ENCRYPT THIS DEVICE"
    append_frag \
        "CONFIG_SECURE_FLASH_ENC_ENABLED=y" \
        "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT=y"
}

cmd_secure_boot_key() {
    if [ -f "$KEY" ]; then
        c_ylw "Signing key already exists: $KEY"
        c_ylw "Refusing to overwrite it - that would orphan any device already"
        c_ylw "signed with it, permanently."
        exit 1
    fi
    command -v espsecure.py >/dev/null 2>&1 || {
        c_red "espsecure.py not found. Run: . \$IDF_PATH/export.sh"; exit 1; }
    espsecure.py generate_signing_key --version 2 "$KEY"
    chmod 600 "$KEY"
    c_grn "generated $KEY"
    echo
    c_red "BACK THIS UP OFFLINE NOW."
    c_red "Lose it and every device signed with it can never be updated again."
    echo "It is git-ignored; keep it out of the repository."
}

cmd_secure_boot() {
    [ -f "$KEY" ] || { c_red "No signing key. Run: $0 secure-boot-key"; exit 1; }
    c_bold "Secure Boot v2"
    echo "After the first boot of a signed image, this device will refuse to run"
    echo "any firmware not signed by $KEY."
    echo
    c_red "There is no way to disable Secure Boot v2 afterwards. None."
    c_red "If you lose the key, the device can never be updated again."
    echo
    require_phrase "SECURE BOOT IS PERMANENT"
    append_frag \
        "CONFIG_SECURE_BOOT=y" \
        "CONFIG_SECURE_BOOT_V2_ENABLED=y" \
        "CONFIG_SECURE_BOOT_SIGNING_KEY=\"secure_boot_signing_key.pem\""
}

cmd_encrypt_release() {
    c_bold "Flash encryption - RELEASE mode"
    c_red "This disables UART re-flashing for good. From here the ONLY way to"
    c_red "update the device is a correctly signed OTA image. Do this last, and"
    c_red "only after you have confirmed the device works in development mode."
    echo
    require_phrase "RELEASE MODE IS FINAL"
    append_frag \
        "CONFIG_SECURE_FLASH_ENC_ENABLED=y" \
        "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y"
}

case "${1:-plan}" in
    plan)             cmd_plan ;;
    encrypt-dev)      cmd_encrypt_dev ;;
    secure-boot-key)  cmd_secure_boot_key ;;
    secure-boot)      cmd_secure_boot ;;
    encrypt-release)  cmd_encrypt_release ;;
    *) echo "usage: $0 {plan|encrypt-dev|secure-boot-key|secure-boot|encrypt-release}"; exit 1 ;;
esac
