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
};

#define KNOB_PROFILE_ENTRY(node)                                                                   \
    {                                                                                              \
        .layer = DT_PROP(node, layer),                                                             \
        .mode = DT_ENUM_IDX(node, mode),                                                           \
        .detents_per_rev = DT_PROP(node, detents_per_rev),                                         \
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

struct knob_data {
    const struct device *dev;
    struct k_work_delayable work;

    /* rotation tracking, in raw sensor ticks (1/4096 rev) */
    int32_t last_pos;
    int32_t accum_scaled; /* accumulated (delta * detents_per_rev) */
    const struct knob_profile *last_prof;
    bool pos_valid;

    /* bounded-mode position, in detent steps */
    int32_t bounded_pos;
    int64_t last_endstop_ms;

#if KNOB_HAS_SLIDERS
    struct slider_state sliders[NUM_SLIDERS];
    bool adc_ready;
#endif
};

static const struct device *const knob_sensor = DEVICE_DT_GET(DT_INST_PHANDLE(0, sensor));

#if KNOB_HAS_HAPTICS
static const struct device *const knob_haptics = DEVICE_DT_GET(DT_INST_PHANDLE(0, haptics));
#endif

static struct knob_data knob_data_0;

/* ---------------- haptics ---------------- */

static void haptic_fire(uint8_t effect) {
#if KNOB_HAS_HAPTICS
    if (effect == 0 || !device_is_ready(knob_haptics)) {
        return;
    }
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

/* Returns the scroll multiplier for wheel-scale profiles (>= 1). */
static int32_t slider_process(struct knob_data *data, const struct knob_profile *prof) {
    int32_t multiplier = 1;

    if (!data->adc_ready || prof->slider_role == SLIDER_ROLE_NONE ||
        prof->slider_index >= NUM_SLIDERS) {
        return multiplier;
    }

    struct slider_state *sl = &data->sliders[prof->slider_index];
    int32_t raw;
    if (slider_read_raw(slider_channels[prof->slider_index], &raw) != 0) {
        return multiplier;
    }

    const int32_t deadband = DT_INST_PROP(0, slider_deadband);
    if (sl->valid && KNOB_ABS(raw - sl->raw) < deadband) {
        raw = sl->raw; /* within noise, hold previous */
    }

    switch (prof->slider_role) {
    case SLIDER_ROLE_WHEEL_SCALE:
        multiplier = 1 + (raw * (prof->wheel_scale_max - 1)) / (SLIDER_RAW_MAX + 1);
        break;

    case SLIDER_ROLE_OWN_CONTROL: {
        int32_t bucket = (raw * prof->slider_steps) / (SLIDER_RAW_MAX + 1);
        if (sl->valid && bucket != sl->bucket) {
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
    return multiplier;
}

#else /* !KNOB_HAS_SLIDERS */

static int32_t slider_process(struct knob_data *data, const struct knob_profile *prof) {
    ARG_UNUSED(data);
    ARG_UNUSED(prof);
    return 1;
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
                       int32_t multiplier) {
    if (steps == 0) {
        return;
    }

    switch (prof->mode) {
    case KNOB_MODE_SCROLL:
        input_report_rel(data->dev, prof->input_code, steps * multiplier, true, K_NO_WAIT);
        haptic_fire(prof->haptic_effect);
        break;

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
            input_report_rel(data->dev, prof->input_code, moved * multiplier, true, K_NO_WAIT);
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
        data->last_prof = prof;
        data->accum_scaled = 0;
    }

    if (knob_read_position(&pos) == 0) {
        if (!data->pos_valid) {
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
        data->accum_scaled += delta * (int32_t)MAX(prof->detents_per_rev, 1);
        int32_t steps = data->accum_scaled / TICKS_PER_REV;

        if (steps != 0) {
            data->accum_scaled -= steps * TICKS_PER_REV;
            int32_t multiplier = slider_process(data, prof);
            emit_steps(data, prof, steps, multiplier);
        } else if (prof->slider_role == SLIDER_ROLE_OWN_CONTROL) {
            /* own-control sliders report even when the knob is still */
            slider_process(data, prof);
        }
    } else {
        LOG_DBG("AS5600 read failed");
    }

    /* activity-gated poll rate */
    uint32_t hz = DT_INST_PROP(0, poll_hz);
    if (zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE) {
        hz = DT_INST_PROP(0, idle_poll_hz);
    }
    k_work_reschedule(dwork, K_USEC(1000000 / MAX(hz, 1)));
}

/* ---------------- init ---------------- */

static int knob_engine_init(const struct device *dev) {
    struct knob_data *data = dev->data;

    data->dev = dev;

    if (!device_is_ready(knob_sensor)) {
        LOG_ERR("angle sensor not ready");
        return -ENODEV;
    }

#if KNOB_HAS_SLIDERS
    slider_adc_init(data); /* sliders are best-effort; knob works without them */
#endif

    k_work_init_delayable(&data->work, knob_work_handler);
    /* small delay so I2C + AS5600 settle after power-on */
    k_work_reschedule(&data->work, K_MSEC(50));

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
