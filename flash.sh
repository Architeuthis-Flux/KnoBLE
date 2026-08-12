#!/bin/sh
# Waits for the UF2 bootloader drive (double-tap reset), then flashes.
FW="${1:-$(dirname "$0")/zmk-debug.uf2}"
echo "Waiting for NICENANO... (double-tap the reset button now)"
while [ ! -d /Volumes/NICENANO ]; do sleep 1; done
sleep 1
cp "$FW" /Volumes/NICENANO/zmk.uf2 2>/dev/null
echo "Flashed $FW (the board reboots itself; a cp metadata error here is normal)"
