# KnoBLE

Wireless version of the BaselineDesign **Knob v2.1**: an nRF52840 SuperMini
(nice!nano-clone form factor) in place of the Pro Micro, running
[ZMK](https://zmk.dev) over Bluetooth or USB.

- **AS5600 magnetic knob** with firmware-defined detents and (with a DRV2605L)
  a haptic click per detent — detent count, feel, and function change per mode.
- **Decoupled scroll**: the page tracks the knob finely (240 counts/rev)
  while haptic clicks follow a coarser texture grid (`detents-per-rev`).
- **Two sliders**: a dedicated **speed slider** (÷5…×4, LED color gauge:
  blue = slow … red = fast) and an **assignable pot** (horizontal scroll /
  volume / off, set from the companion app).
- **Remappable keys** and pot function via the **Knob companion app**
  (raw-HID settings channel, QMK-style; changes persist on the device) —
  the app also provides acceleration-free smooth scrolling on macOS.
- Per-layer knob profiles (scroll / volume / bounded-with-endstop).

## Build one (SuperMini retrofit into a Knob v2.1 PCB)

### Hardware steps

1. Replace the Pro Micro with a
   **[SuperMini nRF52840](https://keeb.io/products/supermini-nrf52840-pro-micro-bluetooth-le-ble-controller)**
   (nice!nano-clone footprint; see the
   [nrfmicro wiki's alternatives list](https://github.com/joric/nrfmicro/wiki/Alternatives)
   for the family tree), same orientation.
2. **Move AS5600 + LED power to RAW** (they were designed for the 5 V Pro
   Micro; the SuperMini's VCC is 3.3 V, which browns out the AS5600's 5 V-mode
   LDO). RAW carries ~4.8 V on USB. *Battery caveat:* on battery RAW is the
   cell voltage (3.7–4.2 V) — marginal for 5 V-mode parts. The proper product
   fix is the AS5600 in 3.3 V mode (tie pin 1 VDD5V to pin 2 VDD3V3) and
   3.3 V-happy LEDs (SK6812) on VCC.
3. **Sliders (optional)**: speed slider wiper → the pad marked **029**
   (P0.29), assignable pot wiper → **031** (P0.31); outer legs of both →
   **3.3 V (VCC pin) and GND**. Never feed a wiper from RAW/5 V — the nRF's
   analog absolute max is VDD + 0.3 V, and the top quarter of travel
   saturates the ADC anyway.
   ⚠️ SuperMini clones: the VCC output gate is wired **inverted** vs. a real
   nice!nano (the overlay drives it active-low — measured on this unit). On
   stock polarity the VCC pin silently reads 0 V. If your VCC pin is dead,
   that's this. Genuine nice!nano v2 boards must remove the `&{/EXT_POWER}`
   override from the overlay.
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
| Speed slider wiper (fixed to scroll speed) | A2 ("029") | P0.29 = AIN5 |
| Assignable pot wiper (app-configurable) | A3 ("031") | P0.31 = AIN7 |

Only **A1/A2/A3** are ADC-capable on this footprint (A0/P1.15 is **not**,
despite the silkscreen) — max 3 analog channels.

### Firmware

**Easiest — GitHub Actions:** fork/push this repo; the included workflow
builds `.uf2` artifacts for every entry in `build.yaml`. Download the
`knoble-nice_nano_v2-zmk` artifact.

**Local build:**

```bash
mkdir -p /private/tmp/knoble-ws/config && cp -R config/* /private/tmp/knoble-ws/config/
cd /private/tmp/knoble-ws && python3 -m venv .venv && .venv/bin/pip install west
.venv/bin/west init -l config && .venv/bin/west update
.venv/bin/pip install -r zephyr/scripts/requirements-base.txt
.venv/bin/west zephyr-export
.venv/bin/west build -s zmk/app -b nice_nano_v2 -S zmk-usb-logging -- \
  -DZMK_CONFIG=/private/tmp/knoble-ws/config -DSHIELD=knoble \
  -DZMK_EXTRA_MODULES=<absolute path to this repo>
# firmware: build/zephyr/zmk.uf2
# hires (Windows/Linux) variant: -d build-hires, drop -S zmk-usb-logging,
# append -DCONFIG_ZMK_POINTING_SMOOTH_SCROLLING=y
```

Needs cmake, ninja, dtc, and a Zephyr SDK ARM toolchain (installed at
`~/zephyr-sdk-0.17.0`, found via the CMake user package registry). Drop
`-S zmk-usb-logging` for a production build without the debug console.
Gotchas relearned the hard way: `west zephyr-export` is required (passing
`-DZephyr_DIR` fails on macOS — `/tmp` is a symlink to `/private/tmp` and
Zephyr's package version check compares workspace paths); the venv needs
`requirements-base.txt` on top of west (`elftools` errors otherwise); and
`/tmp` is wiped on reboot, so expect to rebuild the workspace.

### Flashing

- **First time**: double-tap the SuperMini's reset button → a `NICENANO`
  USB drive mounts → copy the `.uf2` onto it. (macOS `cp` reports a metadata
  error as the board reboots mid-copy — that's success, not failure.)
- **Ever after**: **hold both keys for 3 seconds** → board reboots into the
  bootloader drive itself. `./flash.sh` waits for the drive and copies
  automatically.

### Host setup (macOS — the Knob companion app)

Install the **Knob companion app** (Knob-App repo: Qt/C++, menu-bar). It
reads the knob's rotation as **raw HID counts** — before macOS can apply
its scroll-acceleration curve — and re-emits buttery, strictly-linear
continuous scrolling. It's also where you remap the keys and the
assignable pot (knob plugged in over USB). Grant **Accessibility** and
**Input Monitoring** on first run; the app walks you through both.

Without the app the knob still works everywhere as a plain mouse wheel
(with the OS's usual chunky acceleration). LinearMouse can approximate the
glide if you can't run the app — but never run both at once, they fight
over the same events (the app warns if it detects a LinearMouse scheme
for the knob).

### Host setup / testing (Windows — hi-res build, no software needed)

Windows honors the HID Resolution Multiplier natively, so the `knoble_hires`
firmware gets smooth, acceleration-free scrolling with **zero host
software**. To test:

1. **Flash `zmk-hires.uf2`** (repo root, or the `knoble_hires` CI artifact):
   hold both keys 3 s → copy the file onto the `NICENANO` drive.
2. **Pair over Bluetooth** (or just plug in USB — test both if you can;
   they're separate HID transports).
3. Scroll in something smooth-scroll-capable (Chrome/Edge, VS Code, Word).
   Expect fluid sub-notch movement, no stepping, no speed-up on fast spins.
4. **Linearity check**: one slow revolution and one fast revolution should
   travel the *same distance*. That's the whole point.
5. **Pacing note**: the firmware is tuned for the macOS app (240 counts/rev),
   which reads fast on Windows — pull the speed slider down (÷5 ≈ 48/rev)
   for comfortable pacing. A per-OS pacing profile is an open item.
6. **Failure mode to report**: everything ~16× too fast/far means that
   transport didn't honor the multiplier — note BLE vs USB and file it.

Key remap and pot settings live **on the device**, so mappings made from
the macOS app carry over to Windows; there's no Windows GUI yet (planned —
the settings channel is plain hidapi).

⚠️ **Do not pair the hires build with a Mac** — macOS ignores the
multiplier and scrolls 16× too fast. For macOS, flash `zmk-settings.uf2`.
Linux: same hi-res story as Windows (libinput honors the multiplier).
Full findings: `docs/scroll-feel-handoff.md`.

## Controls

| Input | Action (defaults — keys and pot are remappable in the app) |
|---|---|
| Knob | scroll (speed from the speed slider), tracks the page continuously |
| Left key | previous track *(app-remappable: media, modifiers ⌘⇧⌃⌥, F13–F24…)* |
| Middle key tap / hold | play-pause *(tap app-remappable)* / momentary layer 1 |
| Right key (if populated) | next track *(app-remappable)* |
| Layer 1 + knob | volume, finer detents |
| Layer 1 + left key | toggle USB ↔ BLE output |
| Speed slider (029) | scroll speed ÷5…×4, shown as LED color |
| Assignable pot (031) | horizontal scroll / volume / off — set in the app |
| Hold both keys 3 s | UF2 bootloader (flashing) |

## Tuning (in `boards/shields/knoble/knoble.overlay`, rebuild to apply)

Keys, pot function, per-role sensitivities, and the speed range are
**runtime settings** (companion app → saved on the knob). The dials below
are compile-time:

| Dial | Meaning | Current |
|---|---|---|
| `lines-per-rev` | wheel counts per rev at ×1 (high on purpose: fine steps for the host smoother) | 240 |
| `detents-per-rev` | haptic click grid; output step for volume/bounded | 16 (scroll), 48 (volume) |
| `wheel-scale-max` / `wheel-scale-min-div` | speed range *defaults* (runtime override via app) | ×4 / ÷5 |
| `slider-curve-power` | speed slider response: 1 linear, 2–3 bias slow | 1 |
| `settings-pot-index` | which slider is the app-assignable one | 0 (the 031 pot) |
| `wheel-report-interval-ms` | HID report pooling window (mouse cadence) | 8 |
| `wheel-queue-max` | max pooled counts (drops excess on wild flicks) | 160 |
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
- Runtime settings cover keys + pot roles/sensitivity (raw-HID channel, USB
  only for now — BLE transport is a follow-up). Detent count / haptic
  strength are still compile-time; next candidates for the channel.
- Battery-life pass pending: underglow idle auto-off is disabled for
  bring-up, and knob polling at 250 Hz hasn't been power-profiled.
- SuperMini batch quirks: sleep-current leaks (5.6K pull-up / W5 diode),
  32 kHz crystal failures (`CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y` works
  around), battery reads pegged at 100% while charging (cosmetic).
