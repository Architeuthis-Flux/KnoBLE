/*
 * KnoBLE knob engine
 *
 * Polls an AS5600 absolute magnetic angle sensor over the Zephyr sensor API,
 * quantizes rotation into virtual detents, and per detent:
 *   - emits a relative input event (scroll/bounded modes), or taps a pair of
 *     ZMK keycodes (keycode mode), and
 *   - fires a DRV2605 haptic waveform effect.
 *
 * Profiles (devicetree child nodes) are selected by the highest active ZMK
 * layer. Optional slider potentiometers on SAADC channels either scale the
 * wheel output or act as their own quantized control, per profile.
 *
 * This is the ZMK port of the QMK encoder_driver_task() from
 * qmk_firmware/keyboards/baselinedesign/knobv2_1 (Knob-Wireless branch).
 */

#define DT_DRV_COMPAT baseline_knob_engine

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

#include <zmk/keymap.h>
#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

#if DT_INST_NODE_HAS_PROP(0, slider_channels)
#include <zephyr/drivers/adc.h>
#include <hal/nrf_saadc.h>
#define KNOB_HAS_SLIDERS 1
#else
#define KNOB_HAS_SLIDERS 0
#endif

#if DT_INST_NODE_HAS_PROP(0, haptics)
#include <zmk/drivers/drv2605.h>
#define KNOB_HAS_HAPTICS 1
#else
#define KNOB_HAS_HAPTICS 0
#endif

#if DT_INST_NODE_HAS_PROP(0, led_strip)
#include <zephyr/drivers/led_strip.h>
#define KNOB_HAS_LEDS 1
#define LED_CHAIN DT_PROP(DT_INST_PHANDLE(0, led_strip), chain_length)
#else
#define KNOB_HAS_LEDS 0
#endif

LOG_MODULE_REGISTER(knob_engine, CONFIG_KNOB_ENGINE_LOG_LEVEL);

/* AS5600 raw resolution: 4096 ticks per revolution. */
#define TICKS_PER_REV 4096
#define HALF_REV_TICKS 2048

/* 12-bit SAADC, gain 1/6, 0.6 V internal ref => 3.6 V full scale.
 * A pot across the 3.3 V rail tops out around this raw value. */
#define SLIDER_RAW_MAX 3750

/* Minimum gap between end-stop buzzes so leaning on the stop doesn't rattle. */
#define ENDSTOP_COOLDOWN_MS 250

#define KNOB_ABS(x) ((x) < 0 ? -(x) : (x))

enum knob_mode { KNOB_MODE_SCROLL, KNOB_MODE_KEYCODE, KNOB_MODE_BOUNDED };
enum slider_role { SLIDER_ROLE_NONE, SLIDER_ROLE_WHEEL_SCALE, SLIDER_ROLE_OWN_CONTROL };

struct knob_profile {
    uint8_t layer;
    uint8_t mode;
    uint16_t detents_per_rev;
    uint16_t lines_per_rev;
    uint8_t haptic_effect;
    uint16_t input_code;
    uint32_t cw_code;
    uint32_t ccw_code;
    int32_t range_min;
    int32_t range_max;
    uint8_t endstop_effect;
    uint8_t slider_role;
    uint8_t slider_index;
    uint16_t slider_input_code;
    uint16_t slider_steps;
    uint8_t slider_effect;
    uint8_t wheel_scale_max;
    uint8_t wheel_scale_min_div;
    uint8_t slider_curve_power;
};

#define KNOB_PROFILE_ENTRY(node)                                                                   \
    {                                                                                              \
        .layer = DT_PROP(node, layer),                                                             \
        .mode = DT_ENUM_IDX(node, mode),                                                           \
        .detents_per_rev = DT_PROP(node, detents_per_rev),                                         \
        .lines_per_rev = DT_PROP(node, lines_per_rev),                                         \
        .haptic_effect = DT_PROP(node, haptic_effect),                                             \
        .input_code = DT_PROP(node, input_code),                                                   \
        .cw_code = DT_PROP(node, cw_code),                                                         \
        .ccw_code = DT_PROP(node, ccw_code),                                                       \
        .range_min = DT_PROP(node, range_min),                                                     \
        .range_max = DT_PROP(node, range_max),                                                     \
        .endstop_effect = DT_PROP(node, endstop_effect),                                           \
        .slider_role = DT_ENUM_IDX(node, slider_role),                                             \
        .slider_index = DT_PROP(node, slider_index),                                               \
        .slider_input_code = DT_PROP(node, slider_input_code),                                     \
        .slider_steps = DT_PROP(node, slider_steps),                                               \
        .slider_effect = DT_PROP(node, slider_effect),                                             \
        .wheel_scale_max = DT_PROP(node, wheel_scale_max),                                         \
        .wheel_scale_min_div = DT_PROP(node, wheel_scale_min_div),                                 \
        .slider_curve_power = DT_PROP(node, slider_curve_power),                                   \
    },

static const struct knob_profile profiles[] = {DT_INST_FOREACH_CHILD(0, KNOB_PROFILE_ENTRY)};

BUILD_ASSERT(ARRAY_SIZE(profiles) > 0, "knob-engine needs at least one profile child node");

#if KNOB_HAS_SLIDERS
static const uint8_t slider_channels[] = DT_INST_PROP(0, slider_channels);
#define NUM_SLIDERS ARRAY_SIZE(slider_channels)
struct slider_state {
    int32_t raw;
    int32_t bucket;
    bool valid;
};
#endif

/* Consecutive I2C failures before dropping to the 1 Hz recovery poll. */
#define BUS_FAIL_BACKOFF_THRESHOLD 8

struct knob_data {
    const struct device *dev;
    struct k_work_delayable work;
    uint32_t bus_failures;

    /* rotation tracking, in raw sensor ticks (1/4096 rev) */
    int32_t last_pos;
    int32_t accum_scaled;     /* accumulated (delta * detents_per_rev) — haptics/keycodes */
    int32_t out_accum_scaled; /* accumulated (delta * effective lines_per_rev) — scroll */
    const struct knob_profile *last_prof;
    bool pos_valid;

    /* bounded-mode position, in detent steps */
    int32_t bounded_pos;
    int64_t last_endstop_ms;

    /* wheel-scale speed sampled continuously so the LED gauge is live */
    int32_t cached_speed;
    uint8_t slider_tick;

    /* scroll output pooled between rate-limited HID reports */
    int32_t out_pending;
    int64_t last_flush_ms;

#if KNOB_HAS_SLIDERS
    struct slider_state sliders[NUM_SLIDERS];
    bool adc_ready;
#endif
};

static const struct device *const knob_sensor = DEVICE_DT_GET(DT_INST_PHANDLE(0, sensor));

#if KNOB_HAS_HAPTICS
static const struct device *const knob_haptics = DEVICE_DT_GET(DT_INST_PHANDLE(0, haptics));
#endif

#if KNOB_HAS_LEDS
static const struct device *const knob_leds = DEVICE_DT_GET(DT_INST_PHANDLE(0, led_strip));
#endif

static struct knob_data knob_data_0;

/* The knob gets its own work queue: sensor reads block for the full I2C
 * timeout when the bus is unhappy, and running that on the system workqueue
 * starves ZMK's key scanning, HID, and LED work. Never borrow the system
 * queue for something that can stall on hardware. */
static K_THREAD_STACK_DEFINE(knob_wq_stack, 1024);
static struct k_work_q knob_wq;

/* ---------------- haptics ---------------- */

static void haptic_fire(uint8_t effect) {
#if KNOB_HAS_HAPTICS
    if (effect == 0 || !device_is_ready(knob_haptics)) {
        return;
    }
    LOG_DBG("haptic fire: effect %d", effect);
    struct sensor_value val;

    /* slot 0 = the effect, slot 1 = end-of-sequence, then GO */
    val.val1 = 0;
    val.val2 = effect;
    sensor_attr_set(knob_haptics, SENSOR_CHAN_ALL, (enum sensor_attribute)DRV2605_ATTR_WAVEFORM,
                    &val);
    val.val1 = 1;
    val.val2 = 0;
    sensor_attr_set(knob_haptics, SENSOR_CHAN_ALL, (enum sensor_attribute)DRV2605_ATTR_WAVEFORM,
                    &val);
    sensor_attr_set(knob_haptics, SENSOR_CHAN_ALL, (enum sensor_attribute)DRV2605_ATTR_GO, &val);
#else
    ARG_UNUSED(effect);
#endif
}

/* ---------------- LED speed gauge ---------------- */

#if KNOB_HAS_LEDS

/* hue 0-359 at full saturation -> RGB, value = brightness */
static struct led_rgb hue_to_rgb(uint16_t h, uint8_t v) {
    uint8_t region = (h / 60) % 6;
    uint8_t rem = (h % 60) * 255 / 60;
    uint8_t q = (uint16_t)v * (255 - rem) / 255;
    uint8_t t = (uint16_t)v * rem / 255;

    switch (region) {
    case 0:
        return (struct led_rgb){.r = v, .g = t, .b = 0};
    case 1:
        return (struct led_rgb){.r = q, .g = v, .b = 0};
    case 2:
        return (struct led_rgb){.r = 0, .g = v, .b = t};
    case 3:
        return (struct led_rgb){.r = 0, .g = q, .b = v};
    case 4:
        return (struct led_rgb){.r = t, .g = 0, .b = v};
    default:
        return (struct led_rgb){.r = v, .g = 0, .b = q};
    }
}

#define LED_GAUGE_BRIGHTNESS 60

/* Whole strip shows the speed: 240deg (blue) at the slowest divider,
 * through green around 1x, to 0deg (red) at the top multiplier. */
static void leds_show_speed(const struct knob_profile *prof, int32_t speed) {
    if (!device_is_ready(knob_leds)) {
        return;
    }

    const int32_t max_m = MAX(prof->wheel_scale_max, 1);
    const int32_t min_d = MAX(prof->wheel_scale_min_div, 1);
    const int32_t span = MAX((min_d - 1) + (max_m - 1), 1);
    int32_t n = (speed < 0) ? (min_d + speed) : (min_d - 1 + speed - 1);
    n = CLAMP(n, 0, span);

    struct led_rgb px = hue_to_rgb(240 - (240 * n) / span, LED_GAUGE_BRIGHTNESS);
    struct led_rgb buf[LED_CHAIN];
    for (size_t i = 0; i < LED_CHAIN; i++) {
        buf[i] = px;
    }
    led_strip_update_rgb(knob_leds, buf, LED_CHAIN);
}

static void leds_off(void) {
    if (!device_is_ready(knob_leds)) {
        return;
    }
    struct led_rgb buf[LED_CHAIN] = {0};
    led_strip_update_rgb(knob_leds, buf, LED_CHAIN);
}

#else

static void leds_show_speed(const struct knob_profile *prof, int32_t speed) {
    ARG_UNUSED(prof);
    ARG_UNUSED(speed);
}
static void leds_off(void) {}

#endif /* KNOB_HAS_LEDS */

/* ---------------- profile selection ---------------- */

static const struct knob_profile *active_profile(void) {
    uint8_t layer = zmk_keymap_highest_layer_active();

    for (size_t i = 0; i < ARRAY_SIZE(profiles); i++) {
        if (profiles[i].layer == layer) {
            return &profiles[i];
        }
    }
    return &profiles[0];
}

/* ---------------- sliders ---------------- */

#if KNOB_HAS_SLIDERS

static int slider_adc_init(struct knob_data *data) {
    const struct device *adc = DEVICE_DT_GET(DT_NODELABEL(adc));

    if (!device_is_ready(adc)) {
        LOG_WRN("ADC not ready; sliders disabled");
        return -ENODEV;
    }

    for (size_t i = 0; i < NUM_SLIDERS; i++) {
        struct adc_channel_cfg cfg = {
            .gain = ADC_GAIN_1_6,
            .reference = ADC_REF_INTERNAL,
            .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 10),
            .channel_id = slider_channels[i],
            .input_positive = NRF_SAADC_INPUT_AIN0 + slider_channels[i],
        };
        int err = adc_channel_setup(adc, &cfg);
        if (err) {
            LOG_ERR("slider %d: adc_channel_setup failed (%d)", (int)i, err);
            return err;
        }
    }
    data->adc_ready = true;
    return 0;
}

static int slider_read_raw(uint8_t channel, int32_t *out) {
    const struct device *adc = DEVICE_DT_GET(DT_NODELABEL(adc));
    int16_t buf;
    struct adc_sequence seq = {
        .channels = BIT(channel),
        .buffer = &buf,
        .buffer_size = sizeof(buf),
        .resolution = 12,
        /* Never oversample: known-bad interaction on nRF52840 SAADC
         * (see zmk-analog-input-driver README / battery-nrf-vddh). */
        .oversampling = 0,
    };

    int err = adc_read(adc, &seq);
    if (err) {
        return err;
    }
    *out = CLAMP((int32_t)buf, 0, SLIDER_RAW_MAX);
    return 0;
}

/* Returns the signed wheel speed for wheel-scale profiles:
 *   +M -> emit M lines per detent (multiplier)
 *   -D -> emit 1 line per D detents (divider)
 * Never returns 0; 1 means plain 1:1. */
static int32_t slider_process(struct knob_data *data, const struct knob_profile *prof) {
    int32_t speed = 1;

    if (!data->adc_ready || prof->slider_role == SLIDER_ROLE_NONE ||
        prof->slider_index >= NUM_SLIDERS) {
        return speed;
    }

    struct slider_state *sl = &data->sliders[prof->slider_index];
    int32_t raw;
    if (slider_read_raw(slider_channels[prof->slider_index], &raw) != 0) {
        return speed;
    }

    const int32_t deadband = DT_INST_PROP(0, slider_deadband);
    if (sl->valid && KNOB_ABS(raw - sl->raw) < deadband) {
        raw = sl->raw; /* within noise, hold previous */
    }

    switch (prof->slider_role) {
    case SLIDER_ROLE_WHEEL_SCALE: {
        /* Map slider travel onto notches spanning /min_div ... x max_mult,
         * e.g. min-div 5, max 4: /5 /4 /3 /2, x1, x2 x3 x4 */
        const int32_t max_m = MAX(prof->wheel_scale_max, 1);
        const int32_t min_d = MAX(prof->wheel_scale_min_div, 1);
        const int32_t span = (min_d - 1) + (max_m - 1);
        if (span > 0) {
            /* curve power > 1 biases travel toward the slow end: the top
             * multipliers only arrive when the pot is cranked */
            int32_t curved = raw;
            for (uint8_t p = 1; p < MAX(prof->slider_curve_power, 1); p++) {
                curved = (curved * raw) / SLIDER_RAW_MAX;
            }
            int32_t n = (curved * (span + 1)) / (SLIDER_RAW_MAX + 1);
            n = MIN(n, span);
            if (n < min_d - 1) {
                speed = -(min_d - n); /* divider: /min_d at the bottom, /2 mid */
            } else {
                speed = n - (min_d - 1) + 1; /* multiplier: x1 ... x max_m */
            }
        }
        break;
    }

    case SLIDER_ROLE_OWN_CONTROL: {
        int32_t bucket = (raw * prof->slider_steps) / (SLIDER_RAW_MAX + 1);
        if (sl->valid && bucket != sl->bucket) {
            LOG_DBG("slider %d: raw %d -> bucket %d (%+d)", prof->slider_index, raw, bucket,
                    bucket - sl->bucket);
            input_report_rel(data->dev, prof->slider_input_code, bucket - sl->bucket, true,
                             K_NO_WAIT);
            haptic_fire(prof->slider_effect);
        }
        sl->bucket = bucket;
        break;
    }
    default:
        break;
    }

    sl->raw = raw;
    sl->valid = true;
    return speed;
}

#else /* !KNOB_HAS_SLIDERS */

static int32_t slider_process(struct knob_data *data, const struct knob_profile *prof) {
    ARG_UNUSED(data);
    ARG_UNUSED(prof);
    return 1; /* plain 1:1 speed */
}

#endif /* KNOB_HAS_SLIDERS */

/* ---------------- knob detent engine ---------------- */

static void tap_keycode(uint32_t encoded) {
    if (encoded == 0) {
        return;
    }
    int64_t now = k_uptime_get();
    raise_zmk_keycode_state_changed_from_encoded(encoded, true, now);
    raise_zmk_keycode_state_changed_from_encoded(encoded, false, now);
}

static void emit_steps(struct knob_data *data, const struct knob_profile *prof, int32_t steps,
                       int32_t speed) {
    if (steps == 0) {
        return;
    }

    switch (prof->mode) {
    case KNOB_MODE_KEYCODE:
        for (int32_t i = 0; i < KNOB_ABS(steps); i++) {
            tap_keycode(steps > 0 ? prof->cw_code : prof->ccw_code);
        }
        haptic_fire(prof->haptic_effect);
        break;

    case KNOB_MODE_BOUNDED: {
        int32_t target = CLAMP(data->bounded_pos + steps, prof->range_min, prof->range_max);
        int32_t moved = target - data->bounded_pos;
        data->bounded_pos = target;

        if (moved != 0) {
            /* bounded mode ignores sub-1x speeds; dividers make no sense
             * against a hard-clamped position */
            input_report_rel(data->dev, prof->input_code, moved * MAX(speed, 1), true, K_NO_WAIT);
            haptic_fire(prof->haptic_effect);
        }
        if (moved != steps) { /* clipped at an end-stop */
            int64_t now = k_uptime_get();
            if (now - data->last_endstop_ms > ENDSTOP_COOLDOWN_MS) {
                data->last_endstop_ms = now;
                haptic_fire(prof->endstop_effect);
            }
        }
        break;
    }
    default:
        break;
    }
}

/* Drip pooled scroll output as UNIT (+/-1) events, one per
 * wheel-report-interval-ms. Hosts that replace wheel events with a fixed
 * pixel distance (LinearMouse pixel mode) only honor the event's sign, so
 * speed must be carried by event RATE, not magnitude. The queue is capped:
 * excess is dropped so the page never keeps coasting after the knob stops. */
static void wheel_flush(struct knob_data *data, bool force) {
    ARG_UNUSED(force);
    if (data->out_pending == 0 || data->last_prof == NULL) {
        return;
    }
    int64_t now = k_uptime_get();
    if ((now - data->last_flush_ms) < DT_INST_PROP(0, wheel_report_interval_ms)) {
        return;
    }

    int32_t step = data->out_pending > 0 ? 1 : -1;
    input_report_rel(data->dev, data->last_prof->input_code, step, true, K_NO_WAIT);
    data->out_pending -= step;
    data->last_flush_ms = now;
}

static int knob_read_position(int32_t *out) {
    struct sensor_value val;
    int err = sensor_sample_fetch(knob_sensor);
    if (err) {
        return err;
    }
    err = sensor_channel_get(knob_sensor, SENSOR_CHAN_ROTATION, &val);
    if (err) {
        return err;
    }
    /* The in-tree ams_as5600 driver (pinned Zephyr) encodes:
     *   val1 = whole degrees, val2 = remainder in 1/4096-degree units,
     * i.e. raw_position * 360 == val1 * 4096 + val2.
     * Recover the raw 0..4095 tick position exactly.
     * NOTE: revisit if the driver moves to standard micro-degree val2. */
    *out = ((val.val1 * TICKS_PER_REV) + val.val2) / 360;
    return 0;
}

static void knob_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct knob_data *data = CONTAINER_OF(dwork, struct knob_data, work);

    const struct knob_profile *prof = active_profile();
    int32_t pos;

    /* switching modes drops any partial rotation toward the next detent */
    if (prof != data->last_prof) {
        wheel_flush(data, true); /* drain output under the old profile's code */
        LOG_INF("profile switch -> layer %d (mode %d, %d detents/rev, effect %d)", prof->layer,
                prof->mode, prof->detents_per_rev, prof->haptic_effect);
        data->last_prof = prof;
        data->accum_scaled = 0;
        data->out_accum_scaled = 0;
        if (prof->slider_role == SLIDER_ROLE_WHEEL_SCALE) {
            leds_show_speed(prof, data->cached_speed);
        } else {
            leds_off(); /* gauge only means something in wheel-scale profiles */
        }
    }

    /* Sample the slider continuously (~30 Hz) so the LED gauge tracks the
     * pot live, not just when the knob moves. */
    if (prof->slider_role == SLIDER_ROLE_WHEEL_SCALE && (data->slider_tick++ % 8) == 0) {
        int32_t speed = slider_process(data, prof);
        if (speed != data->cached_speed) {
            data->cached_speed = speed;
            LOG_INF("speed -> %+d", speed);
            leds_show_speed(prof, speed);
        }
    }

    if (knob_read_position(&pos) == 0) {
        if (data->bus_failures >= BUS_FAIL_BACKOFF_THRESHOLD) {
            LOG_INF("AS5600 recovered after %u failed reads", data->bus_failures);
            data->pos_valid = false; /* stale last_pos, resync */
        }
        data->bus_failures = 0;

        if (!data->pos_valid) {
            LOG_INF("AS5600 online, initial position %d ticks", pos);
            data->last_pos = pos;
            data->pos_valid = true;
        }

        int32_t delta = pos - data->last_pos;
        data->last_pos = pos;

        /* shortest-path wraparound (4095 <-> 0 crossing) */
        if (delta > HALF_REV_TICKS) {
            delta -= TICKS_PER_REV;
        } else if (delta < -HALF_REV_TICKS) {
            delta += TICKS_PER_REV;
        }
#if DT_INST_PROP(0, invert)
        delta = -delta;
#endif
        /* exact integer detent quantizer: accumulate delta * detents, one
         * step per full TICKS_PER_REV of accumulated product */
        /* detent quantizer: drives haptics, and output for keycode/bounded */
        data->accum_scaled += delta * (int32_t)MAX(prof->detents_per_rev, 1);
        int32_t steps = data->accum_scaled / TICKS_PER_REV;
        if (steps != 0) {
            data->accum_scaled -= steps * TICKS_PER_REV;
        }

        if (prof->mode == KNOB_MODE_SCROLL) {
            /* Scroll output runs on its own, finer quantizer so the page
             * tracks the knob between detent clicks. Slider speed scales
             * the effective resolution: x2 doubles lines/rev, /2 halves. */
            const int32_t speed = data->cached_speed != 0 ? data->cached_speed : 1;
            int32_t lpr = prof->lines_per_rev > 0 ? prof->lines_per_rev : prof->detents_per_rev;
            lpr = (speed >= 1) ? (lpr * speed) : MAX(lpr / -speed, 1);

            data->out_accum_scaled += delta * lpr;
            int32_t lines = data->out_accum_scaled / TICKS_PER_REV;
            if (lines != 0) {
                data->out_accum_scaled -= lines * TICKS_PER_REV;
                data->out_pending += lines; /* wheel_flush() drips the events */
                const int32_t qmax = DT_INST_PROP(0, wheel_queue_max);
                data->out_pending = CLAMP(data->out_pending, -qmax, qmax);
            }
            if (steps != 0) {
                /* detents are texture only in scroll mode */
                LOG_DBG("detent %+d at pos %d (speed %+d)", steps, pos, speed);
                haptic_fire(prof->haptic_effect);
            }
        } else if (steps != 0) {
            int32_t speed = slider_process(data, prof);
            LOG_DBG("detent %+d at pos %d (mode %d, speed %+d)", steps, pos, prof->mode, speed);
            emit_steps(data, prof, steps, speed);
        } else if (prof->slider_role == SLIDER_ROLE_OWN_CONTROL) {
            /* own-control sliders report even when the knob is still */
            slider_process(data, prof);
        }
        wheel_flush(data, false);
    } else {
        data->bus_failures++;
        if (data->bus_failures == BUS_FAIL_BACKOFF_THRESHOLD) {
            LOG_WRN("AS5600 unreachable (%u consecutive failures) — backing off to 1 Hz. "
                    "Check I2C wiring, pull-ups, and sensor power.",
                    data->bus_failures);
        }
    }

    /* Bus dead? Poll at 1 Hz until it comes back so a broken sensor can't
     * monopolize the bus or this thread. Otherwise activity-gated rate. */
    if (data->bus_failures >= BUS_FAIL_BACKOFF_THRESHOLD) {
        k_work_reschedule_for_queue(&knob_wq, dwork, K_SECONDS(1));
        return;
    }

    uint32_t hz = DT_INST_PROP(0, poll_hz);
    if (zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE) {
        hz = DT_INST_PROP(0, idle_poll_hz);
    }
    k_work_reschedule_for_queue(&knob_wq, dwork, K_USEC(1000000 / MAX(hz, 1)));
}

/* ---------------- init ---------------- */

static int knob_engine_init(const struct device *dev) {
    struct knob_data *data = dev->data;

    data->dev = dev;
    data->cached_speed = 1;

    if (!device_is_ready(knob_sensor)) {
        LOG_ERR("angle sensor not ready");
        return -ENODEV;
    }

#if KNOB_HAS_SLIDERS
    slider_adc_init(data); /* sliders are best-effort; knob works without them */
#endif

    k_work_queue_start(&knob_wq, knob_wq_stack, K_THREAD_STACK_SIZEOF(knob_wq_stack),
                       K_PRIO_PREEMPT(10), NULL);
    k_thread_name_set(&knob_wq.thread, "knob_engine");

    k_work_init_delayable(&data->work, knob_work_handler);
    /* small delay so I2C + AS5600 settle after power-on */
    k_work_reschedule_for_queue(&knob_wq, &data->work, K_MSEC(50));

    LOG_INF("knob engine up: %d profiles, %d sliders", (int)ARRAY_SIZE(profiles),
#if KNOB_HAS_SLIDERS
            (int)NUM_SLIDERS
#else
            0
#endif
    );
    return 0;
}

DEVICE_DT_INST_DEFINE(0, knob_engine_init, NULL, &knob_data_0, NULL, POST_KERNEL,
                      CONFIG_KNOB_ENGINE_INIT_PRIORITY, NULL);
