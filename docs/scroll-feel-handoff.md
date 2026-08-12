# Scroll-feel: state, findings, and the cross-platform driver question

Handoff doc for continuing scroll-feel work on KnoBLE. Self-contained: assumes
no context beyond this repo. Written 2026-08-12 after the initial bring-up
session got the feel from "unusable" to "really good on macOS with LinearMouse."

## The goal

The knob should feel **rock solid**: page glued to rotation at every speed,
no visible jumps, no OS-added inertia or acceleration — the flywheel's own
physics is the only dynamics allowed. And eventually: that feel on **macOS,
Windows, and Linux**, ideally without asking users to hand-configure
third-party tools.

## Current state (good on macOS)

**Firmware** (`modules/knob-engine/`, config in
`boards/shields/knoble/knoble.overlay`): emulates a normal mouse wheel —
wheel *counts* per HID report, pooled at 8 ms (125 Hz mouse cadence, silent
when idle), 48 counts/rev at ×1, analog slider scales counts ÷5…×4,
`wheel-queue-max = 32` drops excess so flicks can't bank scroll that outlives
the knob's rotation. Strictly linear: rotation → counts, no curves.

**Host (macOS)**: LinearMouse with the "blessed block" (see README): distance
`auto`, acceleration `1`, speed `0`, **smoothed engine on**
(easeOutQuartic, response 2, speed 1.1, acceleration 0.17, inertia 0,
bouncing true). The smoothed engine re-posts our discrete clicks as animated
continuous scrolling — that's where the glide comes from.

User verdict: "feels sooo much better." Remaining itches to explore: whatever
jumps/inertia remain live in the LinearMouse smoothed parameters and the
48/rev quantization (see Open Questions).

## Hard-won facts — do not relearn these

### macOS
- macOS wheel acceleration keys on **sustained event rate**, not just
  magnitude. Bursty manual turning never ramps; a coasting flywheel's long
  steady stream ramps hard (page speeds up while the knob physically slows —
  "double acceleration" against the knob's own inertia).
- `defaults write -g com.apple.scrollwheel.scaling -1` = global linear, but
  affects every wheel (user rejected: mouse should keep its accel).
- A HID wheel device **cannot emit continuous/pixel scroll events**; those
  are host-synthesized (trackpad drivers, LinearMouse, Mos). Any "glide"
  requires host software. Firmware-only = chunky line scrolling with the OS
  curve.

### LinearMouse (source-verified, LinearScrollingVerticalTransformer.swift + ScrollingAccelerationSpeedAdjustmentTransformer.swift)
- Fixed `distance` modes (lines **and** pixels) use **only the event's
  sign**; magnitude is honored solely for Logitech hi-res devices. A device
  that packs values >1 into events gets them silently discarded → speed must
  be carried by event *rate* in those modes.
- `scrolling.acceleration` is a **plain multiplier** (applied when ≠ 1):
  1 = neutral, **0 = ×0 = scrolling completely dead** (classic footgun).
- `scrolling.speed` is a per-event *additive* nudge — avoid, keep 0.
- The **smoothed engine** is the only piece that produces continuous glide.
- The **GUI rewrites the whole scheme** when any slider for the device is
  touched — hand-edited JSON (e.g. pixel distances) gets clobbered. Edit
  `~/.config/linearmouse/linearmouse.json` directly; it hot-reloads.

### Approaches tried and abandoned (in git history, don't re-invent)
1. **Firmware velocity ceiling** (cap lines/report, drop excess): punished
   slow manual turning, barely dented flick ramps. Removed.
2. **Quadratic/cubic slider curve**: same failure mode — masking a host
   curve in firmware always taxes the manual case.
3. **±1-unit drip at fixed rate + LinearMouse pixel mode** (`"10px"`):
   actually linear and smooth, but: hard top speed = event rate ceiling,
   BLE chatter at 125–200 ev/s, slider only works via rate, and pixel mode
   still needed LinearMouse anyway. Superseded by mouse emulation + smoothed
   engine, which feels better and is simpler.
4. LinearMouse `acceleration: 0` to "disable acceleration" → kills all
   scrolling (see above).

### Firmware architecture notes (knob-engine)
- Scroll output resolution (`lines-per-rev`) is **decoupled** from the
  haptic detent grid (`detents-per-rev`) — output can be arbitrarily fine
  without changing click feel. Currently 48 vs 16.
- Poll loop runs on a **dedicated work queue** (a stuck I2C bus once starved
  ZMK's kscan/HID/LEDs via the system workqueue — never poll hardware on the
  system queue), with 1 Hz backoff when the AS5600 is unreachable.
- The AS5600 driver in the pinned Zephyr returns val2 in 1/4096-degree units
  (nonstandard); `knob_read_position()` reconstructs raw ticks. Detent and
  output quantizers use exact integer accumulation (delta × rate / 4096).

## Tuning dials (current values)

| Where | Dial | Value | Effect |
|---|---|---|---|
| overlay | `lines-per-rev` | 48 | counts/rev at ×1; higher = finer + faster |
| overlay | `detents-per-rev` | 16 | haptic click grid (feel, not output) |
| overlay | `wheel-report-interval-ms` | 8 | report cadence |
| overlay | `wheel-queue-max` | 32 | anti-banking clamp |
| overlay | `wheel-scale-max` / `-min-div` | 4 / 5 | slider range |
| LinearMouse | smoothed.response | 2 | lower = snappier, higher = floatier |
| LinearMouse | smoothed.speed | 1.1 | animation gain |
| LinearMouse | smoothed.acceleration | 0.17 | engine's internal accel |
| LinearMouse | smoothed.inertia | 0 | keep 0 — no coast beyond input |
| LinearMouse | smoothed.bouncing | true | edge bounce; consider false |

Iteration loop: edit → `west build` (see README) → hold both keys 3 s →
auto-flash via `flash.sh` → judge. LinearMouse JSON edits apply instantly.

## Open questions for the next session

1. **Characterize the remaining "jumps"**: are they 48/rev quantization
   (fix: raise `lines-per-rev`, the smoothed engine happily eats finer
   input), smoothed-engine response tuning, or BLE report jitter? The debug
   console (`screen /dev/cu.usbmodem* 115200`) timestamps every emission.
2. **Inertia audit**: `inertia: 0` and `bouncing: true` — does bouncing read
   as unwanted motion? Try false.
3. **Slider↔smoothing interaction**: at ×4 the counts get big; does the
   smoothed engine handle bursts gracefully or overshoot?
4. Does the **volume layer** (keycode mode) need its own feel pass?

## The cross-platform app/driver question

Why it exists: every OS distorts wheel input differently, and none of it is
fixable from a standard HID device. The per-OS landscape:

| OS | Native wheel handling | Path to rock-solid |
|---|---|---|
| macOS | rate-keyed acceleration, line-quantized; ignores HID Resolution Multiplier | event-tap app (CGEventTap + Accessibility permission) synthesizing continuous scroll — exactly what LinearMouse/Mos do |
| Windows | 120-per-notch WM_MOUSEWHEEL; **honors HID Resolution Multiplier** (hi-res wheels scroll smoothly natively); per-app smooth scrolling varies | possibly **zero host software needed** if firmware implements hi-res wheel; else a low-level hook app (SendInput) |
| Linux | libinput **honors HID Resolution Multiplier** (`REL_WHEEL_HI_RES`); no forced acceleration | likely **zero host software needed** with hi-res wheel firmware |

**Investigate first: HID Resolution Multiplier in firmware.** ZMK has
`CONFIG_ZMK_POINTING_SMOOTH_SCROLLING` (hi-res wheel). If enabled, Windows
and Linux may get smooth, acceleration-free fine scrolling *natively* —
two of three platforms solved with one Kconfig. macOS ignores the multiplier
(and would see 120× values or fall back — must test carefully, dual-boot the
config behind a build variant first). Check ZMK v0.3's implementation
maturity and how values are scaled per transport.

**The companion app (product-grade option).** One small app per platform
(or one Rust/Swift-per-backend codebase) that:
- synthesizes smooth continuous scrolling from our events (macOS: CGEventTap;
  Windows: LL mouse hook + SendInput; Linux: likely unnecessary),
- talks raw HID to the knob (settings channel) to expose **runtime controls**
  — detent count, speed range, haptic strength — recovering what was lost
  leaving VIA behind. ZMK Studio can't do custom settings; a companion app
  can. Prior art: badjeff's `zmk-hid-io` module + `zmk-companion-macos`
  (host↔ZMK raw HID demo).

**Ship-now alternative**: README documents LinearMouse (macOS). Equivalents
worth vetting for docs: Mos (macOS, free), and whatever the current Windows
smooth-scroll standard is (research needed — candidates historically:
X-Mouse-adjacent tools; verify current state).

Recommended order: (1) hi-res wheel experiment on Windows/Linux, (2) macOS
stays LinearMouse for now, (3) companion app only when the PCB/product
settles — it's also the runtime-settings answer, so design them together.

## Context links

- Firmware repo layout, build, flash, pin map: `../README.md`
- LinearMouse source (behavior ground truth):
  https://github.com/linearmouse/linearmouse — `LinearMouse/EventTransformer/`
- macOS scroll event docs: https://developer.apple.com/documentation/appkit/nsevent/1535387-scrollingdeltay
- discrete-scroll (minimal macOS accel-killer, sign-based like LinearMouse
  line mode): https://github.com/emreyolcu/discrete-scroll
- ZMK pointing/smooth scrolling: https://zmk.dev/docs/keymaps/behaviors/mouse-emulation
  and `CONFIG_ZMK_POINTING_SMOOTH_SCROLLING` in ZMK's Kconfig
- badjeff host-channel prior art: https://github.com/badjeff/zmk-hid-io ·
  https://github.com/badjeff/zmk-companion-macos
