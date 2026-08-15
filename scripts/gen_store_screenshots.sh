#!/bin/bash
# Generate the Rebbble-store main screenshots: resources/store/main_<platform>.png
# for all five platforms (basalt, chalk, diorite, emery, gabbro).
#
# REQUIRES:
#  - a machine WITH a display (the emulator needs X/SDL; on headless machines
#    the display freezes and screenshots never update past the app's first frame)
#  - scripts/apply_pypkjs_patches.sh run once (lets the emulator's JS reach
#    a LAN FreshRSS server)
#  - the phone-side config set once in the Pebble app for the emulator
#    (FreshRSS URL / user / API token), otherwise the app shows
#    "No feeds yet" or the "Set server in phone settings" dialog
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

# pebble screenshot --all-platforms boots each platform's emulator, installs
# the app (which auto-launches it), and captures. The 15 s settle after the
# install (patched into pebble-tool) lets the app fetch the feed tree first.
pebble screenshot --all-platforms --no-open || exit 1

for p in basalt chalk diorite emery gabbro; do
  latest=$(ls -t screenshots/${p}_*.png 2>/dev/null | head -1)
  if [ -n "$latest" ]; then
    cp "$latest" "$STORE/main_${p}.png"
    echo "saved $STORE/main_${p}.png"
  fi
done

echo "Done: $STORE/main_*.png"
