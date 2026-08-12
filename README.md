# KnoBLE

Wireless version of the BaselineDesign **Knob v2.1** — an nRF52840 SuperMini
(nice!nano-clone form factor) replacing the atmega32u4 Pro Micro, running
[ZMK](https://zmk.dev) over Bluetooth, with:

- **AS5600 magnetic knob** with *virtual detents*: rotation is quantized in
  firmware, and a **DRV2605L-driven LRA** fires a haptic click per detent —
  detent count, feel, and function change per mode ("programmable detents").
- **Per-layer knob profiles**: scroll on the base layer, volume with finer
  detents on the hold layer, and a bounded mode (with end-stop buzz) available.
- Optional **analog slider potentiometers** that either scale the knob's
  scroll speed or act as their own control, per profile.
- 3 keys (media prev/play/next by default) + 3× WS2812 LEDs, like the v2.1.

> **Why not QMK?** Mainline QMK has no nRF52840/BLE support. This repo replaces
> the QMK firmware (`qmk_firmware` branch `Knob-Wireless` remains as the wired
> v2.1 reference).

## Repo layout

| Path | What |
|---|---|
| `config/west.yml` | Pins ZMK **v0.3** + two community modules (SHA-pinned) |
| `boards/shields/knoble/` | Shield: pins, sensors, keymap, knob profiles |
| `modules/knob-engine/` | Custom Zephyr module: AS5600 → detents → events + haptics |
| `build.yaml` | CI build matrix (normal + ZMK Studio variant) |

## Building

### GitHub Actions (primary)

Push to GitHub — the included workflow builds `.uf2` artifacts for every entry
in `build.yaml`. Download the artifact, double-tap reset on the SuperMini to
enter the UF2 bootloader, and drag the file onto the `NICENANO` drive.

### Locally

```bash
python3 -m venv .venv && .venv/bin/pip install west
mkdir -p /tmp/knoble-ws/config && cp -R config/* /tmp/knoble-ws/config/
cd /tmp/knoble-ws
west init -l config && west update
west build -s zmk/app -b nice_nano_v2 -- \
  -DZMK_CONFIG=/tmp/knoble-ws/config \
  -DSHIELD=knoble \
  -DZMK_EXTRA_MODULES=<absolute path to this repo>
```

Requires cmake, ninja, dtc, and the Zephyr SDK ARM toolchain
(`ZEPHYR_SDK_INSTALL_DIR`). The firmware lands at
`build/zephyr/zmk.uf2`.

## Pin map (⚠️ read before routing the PCB)

| Function | Pro micro pos | nRF52840 |
|---|---|---|
| I2C SDA / SCL — AS5600 (0x36) + DRV2605L (0x5A) | D2 / D3 | P0.17 / P0.20 |
| Keys 1–3 (direct to GND) | D4, D5, D6 | P0.22, P0.24, P1.00 |
| Sliders (ADC) | **A1, A2, A3 only** | P0.02 (AIN0), P0.29 (AIN5), P0.31 (AIN7) |
| WS2812 data (3 LEDs) | D1 | P0.06 |

**PCB traps on the SuperMini / pro-micro footprint:**

- **A0 (P1.15) is NOT ADC-capable** despite the silkscreen — never route a
  slider there. Only A1/A2/A3 reach SAADC inputs → **max 3 sliders** without
  an external ADC.
- The SuperMini's extra pins (P1.01/P1.02/P1.07) are digital-only (port 1).
- D5 (P0.24) has an unpopulated battery-divider footprint on some SuperMini
  batches — fine as a key pin, but know it's there.
- Keys are configured active-low with internal pull-ups: switch to GND.

## How the knob works (`modules/knob-engine`)

A work-queue loop polls the AS5600 (250 Hz active / 25 Hz idle), accumulates
wraparound-safe angle deltas, and divides by the active profile's
`detents-per-rev`. Each detent step:

1. emits the profile's output — mouse wheel (`scroll`), a keycode tap like
   `C_VOL_UP` (`keycode`), or a clamped value with end-stop buzz (`bounded`) —
2. fires the profile's DRV2605 waveform effect (TI ROM library 6, LRA).

Profiles live in `boards/shields/knoble/knoble.overlay` as children of the
`knob_engine` node and are selected by the **highest active ZMK layer**.
Slider roles (`wheel-scale` / `own-control`) are configured per profile there
too. Haptic-on-keypress uses `zmk,output-behavior-listener` in the same file.

**Tuning cheat-sheet** (all in the overlay, rebuild to apply):

- Detent feel: `detents-per-rev` (24 = chunky, 96 = fine)
- Click effect: `haptic-effect` — TI DRV2605L waveform library IDs
  (1 = strong click, 7 = soft bump, 26 = sharp tick; see TI datasheet §12.1.2)
- Direction: add `invert;` to the `knob_engine` node
- ZMK Studio builds (`knoble_studio` artifact) allow runtime **keymap** editing,
  but detent profiles are compile-time (Studio has no custom-settings support).

## Bring-up checklist (first hardware)

1. Flash, check the keyboard advertises as **KnoBLE** and the 3 keys work.
2. No knob? Enable in `config/knoble.conf`:
   `CONFIG_ZMK_USB_LOGGING=y` + `CONFIG_KNOB_ENGINE_LOG_LEVEL_DBG=y`, watch
   the USB console — "angle sensor not ready" means I2C wiring/address.
3. Knob scrolls but no clicks → DRV2605 at 0x5A on the same bus; check
   `library = <6>` (LRA) and that the LRA is rated for the DRV2605L defaults —
   run TI auto-calibration once if the response is weak (`DRV2605_MODE_AUTOCAL`).
4. Sliders: uncomment `slider-channels` in the overlay and a `slider-role`
   in a profile. If ADC readings **freeze after ~1 minute**, you've hit the
   known nRF52840 SAADC oversampling clash with ZMK's battery driver — see
   *Known risks* below.
5. Battery: expect 100% while charging over USB (VDDH quirk, cosmetic).

## Known risks / follow-ups

- **SAADC oversampling clash**: ZMK's `battery-nrf-vddh` driver uses
  oversampling, which can wedge other SAADC reads (sliders). Fix order:
  (a) test as-is; (b) override the `vbatt` node with a zero-oversampling
  battery driver in knob-engine; (c) last resort `CONFIG_ZMK_BATTERY_REPORTING=n`.
- **Zephyr upgrades**: `CONFIG_AS5600` was renamed `CONFIG_AMS_AS5600` in
  Zephyr 4.x, and the badjeff modules are SHA-pinned against ZMK v0.3 —
  revisit both when bumping ZMK past v0.3.
- Runtime detent tuning (settings storage + an adjust behavior) is designed
  but not yet implemented — sensitivity changes are compile-time for now.
- SuperMini batch quirks: some units leak ~1 mA in sleep (5.6K pull-up or
  wrong W5 diode — fixable by rework); the 32 kHz crystal fails on some
  batches (`CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y` works around it).
