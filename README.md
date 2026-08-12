# KnoBLE

Wireless version of the BaselineDesign **Knob v2.1**: an nRF52840 SuperMini
(nice!nano-clone form factor) in place of the Pro Micro, running
[ZMK](https://zmk.dev) over Bluetooth or USB.

- **AS5600 magnetic knob** with firmware-defined detents and (with a DRV2605L)
  a haptic click per detent — detent count, feel, and function change per mode.
- **Decoupled scroll**: the page tracks the knob finely (`lines-per-rev`)
  while haptic clicks follow a coarser texture grid (`detents-per-rev`).
- **Speed slider**: an analog pot scales scroll speed (÷5…×4); the three
  WS2812 LEDs are a live color gauge (blue = slow … red = fast).
- 2 media keys, per-layer knob profiles (scroll / volume / bounded-with-endstop).

## Build one (SuperMini retrofit into a Knob v2.1 PCB)

### Hardware steps

1. Replace the Pro Micro with a **SuperMini nRF52840**, same orientation.
2. **Move AS5600 + LED power to RAW** (they were designed for the 5 V Pro
   Micro; the SuperMini's VCC is 3.3 V, which browns out the AS5600's 5 V-mode
   LDO). RAW carries ~4.8 V on USB. *Battery caveat:* on battery RAW is the
   cell voltage (3.7–4.2 V) — marginal for 5 V-mode parts. The proper product
   fix is the AS5600 in 3.3 V mode (tie pin 1 VDD5V to pin 2 VDD3V3) and
   3.3 V-happy LEDs (SK6812) on VCC.
3. **Speed slider (optional)**: pot wiper → the pad marked **031** (P0.31),
   outer legs → **3.3 V (VCC pin) and GND**. Never feed the wiper from RAW/5 V
   — the nRF's analog absolute max is VDD + 0.3 V.
4. **DRV2605L + LRA (optional, for haptic detents)**: DRV2605L on the same
   I2C bus (SDA = D2, SCL = D3), address 0x5A, LRA on its output.

Pin map (pro-micro positions → nRF52840):

| Function | Position | nRF pin |
|---|---|---|
| I2C SDA / SCL (AS5600 0x36, DRV2605L 0x5A) | D2 / D3 | P0.17 / P0.20 |
| Key: previous track | D9 | P1.06 |
| Key: play/pause (hold = layer 1) | D8 | P1.04 |
| Key: next track (unpopulated on v2.1) | D7 | P0.11 |
| WS2812 data (3 LEDs) | D10 | P0.09 (NFC pin, freed in config) |
| Slider pot wiper | A3 ("031") | P0.31 = AIN7 |

Only **A1/A2/A3** are ADC-capable on this footprint (A0/P1.15 is **not**,
despite the silkscreen) — max 3 analog channels.

### Firmware

**Easiest — GitHub Actions:** fork/push this repo; the included workflow
builds `.uf2` artifacts for every entry in `build.yaml`. Download the
`knoble-nice_nano_v2-zmk` artifact.

**Local build:**

```bash
python3 -m venv .venv && .venv/bin/pip install west
mkdir -p /tmp/knoble-ws/config && cp -R config/* /tmp/knoble-ws/config/
cd /tmp/knoble-ws && west init -l config && west update
west build -s zmk/app -b nice_nano_v2 -S zmk-usb-logging -- \
  -DZMK_CONFIG=/tmp/knoble-ws/config -DSHIELD=knoble \
  -DZMK_EXTRA_MODULES=<absolute path to this repo>
# firmware: build/zephyr/zmk.uf2
```

Needs cmake, ninja, dtc, and a Zephyr SDK ARM toolchain
(`ZEPHYR_SDK_INSTALL_DIR`; run `west zephyr-export` once, or pass
`-DZephyr_DIR=<ws>/zephyr/share/zephyr-package/cmake`). Drop
`-S zmk-usb-logging` for a production build without the debug console.

### Flashing

- **First time**: double-tap the SuperMini's reset button → a `NICENANO`
  USB drive mounts → copy the `.uf2` onto it. (macOS `cp` reports a metadata
  error as the board reboots mid-copy — that's success, not failure.)
- **Ever after**: **hold both keys for 3 seconds** → board reboots into the
  bootloader drive itself. `./flash.sh` waits for the drive and copies
  automatically.

### Host setup (macOS — this is where the great scroll feel comes from)

The firmware reports like a normal mouse wheel (counts per report, 48
notches/rev at ×1). The buttery glide comes from
[LinearMouse](https://linearmouse.app) (free, MIT) re-posting those clicks as
smooth continuous scrolling — something a HID wheel device cannot emit
itself. Install it and use this scheme (per-device: your mouse is
unaffected):

```bash
brew install --cask linearmouse
```

`~/.config/linearmouse/linearmouse.json` — the blessed KnoBLE block:

```json
{
  "if": { "device": { "productName": "KnoBLE", "vendorID": "0x1d50", "productID": "0x615e" } },
  "scrolling": {
    "acceleration": { "vertical": 1 },
    "distance": { "vertical": "auto" },
    "speed": { "vertical": 0 },
    "smoothed": {
      "vertical": {
        "enabled": true,
        "preset": "easeOutQuartic",
        "response": 2,
        "speed": 1.1,
        "acceleration": 0.17,
        "inertia": 0,
        "bouncing": true
      }
    }
  }
}
```

⚠️ Touching this device's sliders in the LinearMouse **GUI rewrites the
scheme** — edit the JSON instead (it hot-reloads). Without LinearMouse the
knob still works everywhere as a plain mouse wheel, just without the glide.

## Controls

| Input | Action |
|---|---|
| Knob | scroll (speed from slider), tracks the page continuously |
| Left key | previous track |
| Middle key tap / hold | play-pause / momentary layer 1 |
| Layer 1 + knob | volume, finer detents |
| Layer 1 + left key | toggle USB ↔ BLE output |
| Slider | scroll speed ÷5…×4, shown as LED color |
| Hold both keys 3 s | UF2 bootloader (flashing) |

## Tuning (in `boards/shields/knoble/knoble.overlay`, rebuild to apply)

| Dial | Meaning | Current |
|---|---|---|
| `lines-per-rev` | wheel counts per rev at ×1 (like mouse-wheel notches) | 48 |
| `detents-per-rev` | haptic click grid; output step for volume/bounded | 16 (scroll), 48 (volume) |
| `wheel-scale-max` / `wheel-scale-min-div` | slider speed range | ×4 / ÷5 |
| `slider-curve-power` | pot response: 1 linear, 2–3 bias slow | 1 |
| `wheel-report-interval-ms` | HID report pooling window (mouse cadence) | 8 |
| `wheel-queue-max` | max pooled counts (drops excess on wild flicks) | 32 |
| `haptic-effect` / `endstop-effect` | TI DRV2605L waveform library IDs (datasheet §12.1.2) | 1 / 14 |
| `invert` (on `knob_engine`) | flip rotation direction | unset |

## Debugging

Debug builds (the `zmk-usb-logging` snippet) stream everything to a USB
serial console — detents, speed changes, slider buckets, key events, I2C
health:

```bash
screen /dev/cu.usbmodem* 115200     # exit: ctrl-a k y
```

Bring-up hints: "AS5600 unreachable" → check I2C wiring and sensor power
(an unpowered AS5600 clamps the whole bus). Keys work but LEDs/knob dead →
peripheral power rail. ADC readings frozen after ~1 min → the known nRF52840
SAADC oversampling clash with ZMK's battery driver (see below).

## Known issues / follow-ups

- **SAADC oversampling clash**: ZMK's `battery-nrf-vddh` driver uses
  oversampling, which can wedge slider reads. Order of attack: (a) live with
  it during bring-up; (b) override the `vbatt` node with a zero-oversampling
  battery driver in knob-engine; (c) `CONFIG_ZMK_BATTERY_REPORTING=n` last.
- **Bumping ZMK past v0.3**: badjeff modules are SHA-pinned, and
  `knob_read_position()` relies on the pinned AS5600 driver's value encoding
  — revisit both.
- Runtime detent tuning (settings storage + adjust behavior) is designed but
  not implemented; all tuning is compile-time.
- Battery-life pass pending: underglow idle auto-off is disabled for
  bring-up, and knob polling at 250 Hz hasn't been power-profiled.
- SuperMini batch quirks: sleep-current leaks (5.6K pull-up / W5 diode),
  32 kHz crystal failures (`CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y` works
  around), battery reads pegged at 100% while charging (cosmetic).
