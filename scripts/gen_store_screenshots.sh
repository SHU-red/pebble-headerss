#!/bin/bash
# Generate the Rebbble-store main screenshots: resources/store/main_<platform>.png
# for all five platforms (basalt, chalk, diorite, emery, gabbro).
#
# REQUIRES a machine WITH a display (the emulator needs X/SDL; on headless
# machines the display freezes and the app cannot be driven reliably).
#
# The phone-side config (FreshRSS URL/user/API password) must be set once in
# the Pebble app for the emulator — otherwise the app shows the
# "Set server in phone settings" dialog instead of the feed tree.
#
# Run from the project root:  scripts/gen_store_screenshots.sh
set -u
cd "$(dirname "$0")/.."

PBW=build/pebble-headerss.pbw
STORE=resources/store
mkdir -p "$STORE"

if [ ! -f "$PBW" ]; then
  echo "PBW not found — build first: pebble build"
  exit 1
fi

for p in basalt chalk diorite emery gabbro; do
  echo "=== $p ==="
  # Boot the emulator (starts qemu + pypkjs; needs a display).
  pebble emu-control --emulator "$p" >/tmp/emu_${p}.log 2>&1 &
  EMU_PID=$!
  # Wait for the emulator to come up (the tool prints the QR/URL page).
  sleep 30

  pebble install --emulator "$p" "$PBW" || { echo "install failed on $p"; kill $EMU_PID; continue; }

  # Set the advertising time (10:10) so the sidebar clock reads nicely.
  pebble emu-set-time --emulator "$p" 10 10 2>/dev/null || true

  # Launcher: the app is one DOWN from the watchface; SELECT launches it.
  pebble emu-button --emulator "$p" click down
  sleep 2
  pebble emu-button --emulator "$p" push select
  sleep 1
  pebble emu-button --emulator "$p" release select

  # App start + tree fetch + first full summaries.
  sleep 20

  pebble screenshot --emulator "$p" --no-open "$STORE/main_${p}.png" \
    && echo "saved $STORE/main_${p}.png"

  # Shut this platform's emulator down.
  pebble kill 2>/dev/null || true
  kill $EMU_PID 2>/dev/null || true
  sleep 2
done

echo "Done: $STORE/main_*.png"
