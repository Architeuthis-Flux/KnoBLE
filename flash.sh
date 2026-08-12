#!/bin/sh
# Waits for the UF2 bootloader drive (hold both keys 3 s), then flashes.
# Success is judged by the drive UNMOUNTING (the board rebooting into the
# new firmware), not by cp's exit code — cp always errors mid-copy as the
# board reboots, and once (2026-08-12) it errored while the copy had NOT
# taken, leaving the drive mounted and the old firmware running.
FW="${1:-$(dirname "$0")/zmk-debug.uf2}"
[ -f "$FW" ] || { echo "No such firmware file: $FW"; exit 1; }

echo "Waiting for NICENANO... (hold both keys 3 s, or double-tap reset)"
while [ ! -d /Volumes/NICENANO ]; do sleep 1; done
sleep 1

cp "$FW" /Volumes/NICENANO/zmk.uf2 2>/dev/null

# The board yanks the drive as it reboots; give it a few seconds.
for i in 1 2 3 4 5 6 7 8; do
    [ ! -d /Volumes/NICENANO ] && { echo "Flashed $FW (board rebooting)"; exit 0; }
    sleep 1
done

echo "!! NICENANO is still mounted — the copy did NOT take. Retrying once..."
cp "$FW" /Volumes/NICENANO/zmk.uf2 2>/dev/null
for i in 1 2 3 4 5 6 7 8; do
    [ ! -d /Volumes/NICENANO ] && { echo "Flashed $FW on retry (board rebooting)"; exit 0; }
    sleep 1
done
echo "!! Still mounted. Unplug/replug and try again, or copy by hand:"
echo "   cp '$FW' /Volumes/NICENANO/zmk.uf2"
exit 1
