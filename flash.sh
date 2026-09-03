#!/usr/bin/env bash
# Flash, and KEEP the exact image that was flashed.
#
# A core dump can only be decoded against the ELF that produced it, and the app
# SHA changes on every build - so rebuilding the "same" commit does not give a
# matching image. Twice now a real crash was unreadable because build/dolos.elf
# had already been overwritten. Archive it at the moment of flashing.
set -eo pipefail
PORT="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "no device (hold BOOT while replugging for flash mode)"; exit 1; }
idf.py -p "$PORT" flash
SHA=$(esptool.py --version >/dev/null 2>&1; python3 - <<'PY'
import hashlib,sys
print(hashlib.sha256(open("build/dolos.bin","rb").read()).hexdigest()[:9])
PY
)
STAMP="$(date +%Y%m%d-%H%M%S)-$SHA"
cp build/dolos.elf ".flashed/dolos-$STAMP.elf"
echo "archived .flashed/dolos-$STAMP.elf"
ls -t .flashed/*.elf | tail -n +6 | xargs -r rm -f    # keep the last 5
