/*
 * Runtime knob settings over a vendor raw-HID channel (USB).
 *
 * Transport is shaped exactly like QMK's raw HID: usage page 0xFF60, usage
 * 0x61, 32-byte input/output reports on a second USB HID instance ("HID_1",
 * CONFIG_USB_HID_DEVICE_COUNT=2) — the companion app finds it by that usage
 * page, and the shape leaves the door open to VIA-protocol compatibility.
 * BLE transport is a follow-up; configuring over the cable is v1 (like VIA).
 *
 * Protocol: [cmd, seq, payload...]; replies echo cmd+seq, status at [2].
 * SET stages in RAM (feel it immediately), COMMIT persists to NVS.
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zephyr/usb/usb_device.h>

#include <dt-bindings/zmk/keys.h>

#include "knob_settings.h"

LOG_MODULE_REGISTER(knob_settings, CONFIG_KNOB_ENGINE_LOG_LEVEL);

#define RAW_REPORT_SIZE 32
#define RAW_USAGE_PAGE 0xFF60
#define RAW_USAGE 0x61

enum knob_cmd {
    KNOB_CMD_GET_INFO = 0x01,
    KNOB_CMD_GET_KEY = 0x02,
    KNOB_CMD_SET_KEY = 0x03,
    KNOB_CMD_COMMIT = 0x04,
    KNOB_CMD_RESET = 0x05,
    KNOB_CMD_GET_POT_CFG = 0x06,
    KNOB_CMD_SET_POT_CFG = 0x07,
    KNOB_CMD_GET_POT_VALUE = 0x08,
};

#define KNOB_PROTO_VERSION 2
#define KNOB_STATUS_OK 0
#define KNOB_STATUS_BAD_ARG 1

/* Defaults mirror the shipped keymap: prev / play-pause / next. */
static const uint32_t default_key_codes[KNOB_KEY_SLOTS] = {C_PREV, C_PP, C_NEXT};
/* Pot default mirrors the shipped overlay: speed role, /5 .. x4, 32 steps. */
static const struct knob_pot_cfg default_pot_cfg = {
    .role = KNOB_POT_ROLE_SPEED,
    .speed_max_mult = 4,
    .speed_min_div = 5,
    .steps = 32,
};

static struct {
    uint32_t key_codes[KNOB_KEY_SLOTS];
    struct knob_pot_cfg pot;
} knob_cfg;

/* Live pot sample, engine-thread written, USB-thread read; both 32-bit
 * aligned writes, torn reads are harmless (display only). */
static volatile int32_t pot_raw_latest = -1;
static volatile int16_t pot_semantic_latest;

uint32_t knob_settings_key_code(uint8_t slot) {
    if (slot >= KNOB_KEY_SLOTS) {
        return 0;
    }
    return knob_cfg.key_codes[slot];
}

const struct knob_pot_cfg *knob_settings_pot(void) { return &knob_cfg.pot; }

void knob_settings_note_pot(int32_t raw, int16_t semantic) {
    pot_raw_latest = raw;
    pot_semantic_latest = semantic;
}

/* ---------------- persistence (Zephyr settings / NVS) ---------------- */

static int knob_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                             void *cb_arg) {
    if (settings_name_steq(name, "keys", NULL)) {
        if (len != sizeof(knob_cfg.key_codes)) {
            return -EINVAL;
        }
        int rc = read_cb(cb_arg, knob_cfg.key_codes, sizeof(knob_cfg.key_codes));
        if (rc >= 0) {
            LOG_INF("loaded key slots: %08x %08x %08x", knob_cfg.key_codes[0],
                    knob_cfg.key_codes[1], knob_cfg.key_codes[2]);
            return 0;
        }
        return rc;
    }
    if (settings_name_steq(name, "pot", NULL)) {
        if (len != sizeof(knob_cfg.pot)) {
            return -EINVAL;
        }
        int rc = read_cb(cb_arg, &knob_cfg.pot, sizeof(knob_cfg.pot));
        if (rc >= 0) {
            LOG_INF("loaded pot cfg: role %d, /%d..x%d, %d steps", knob_cfg.pot.role,
                    knob_cfg.pot.speed_min_div, knob_cfg.pot.speed_max_mult, knob_cfg.pot.steps);
            return 0;
        }
        return rc;
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(knob, "knob", NULL, knob_settings_set, NULL, NULL);

static void knob_settings_save(void) {
    settings_save_one("knob/keys", knob_cfg.key_codes, sizeof(knob_cfg.key_codes));
    settings_save_one("knob/pot", &knob_cfg.pot, sizeof(knob_cfg.pot));
}

/* ---------------- raw HID transport (USB, QMK-style) ---------------- */

static const uint8_t raw_hid_desc[] = {
    HID_ITEM(HID_ITEM_TAG_USAGE_PAGE, HID_ITEM_TYPE_GLOBAL, 2), (RAW_USAGE_PAGE & 0xFF),
    (RAW_USAGE_PAGE >> 8),
    HID_USAGE(RAW_USAGE),
    HID_COLLECTION(HID_COLLECTION_APPLICATION),
    /* input: device -> host */
    HID_USAGE(0x62),
    HID_LOGICAL_MIN8(0x00),
    HID_LOGICAL_MAX16(0xFF, 0x00),
    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(RAW_REPORT_SIZE),
    HID_INPUT(0x02),
    /* output: host -> device */
    HID_USAGE(0x63),
    HID_LOGICAL_MIN8(0x00),
    HID_LOGICAL_MAX16(0xFF, 0x00),
    HID_REPORT_SIZE(8),
    HID_REPORT_COUNT(RAW_REPORT_SIZE),
    HID_OUTPUT(0x02),
    HID_END_COLLECTION,
};

static const struct device *raw_hid_dev;
static K_SEM_DEFINE(raw_in_sem, 1, 1);

static void raw_in_ready_cb(const struct device *dev) { k_sem_give(&raw_in_sem); }

static void raw_send_reply(uint8_t *frame) {
    if (!raw_hid_dev) {
        return;
    }
    if (k_sem_take(&raw_in_sem, K_MSEC(50)) != 0) {
        LOG_WRN("raw HID IN endpoint busy, reply dropped");
        return;
    }
    int err = hid_int_ep_write(raw_hid_dev, frame, RAW_REPORT_SIZE, NULL);
    if (err) {
        LOG_WRN("raw HID reply failed (%d)", err);
        k_sem_give(&raw_in_sem);
    }
}

static void handle_frame(const uint8_t *in, uint8_t *reply) {
    const uint8_t cmd = in[0];
    reply[0] = cmd;
    reply[1] = in[1]; /* echo seq */
    reply[2] = KNOB_STATUS_OK;

    switch (cmd) {
    case KNOB_CMD_GET_INFO:
        reply[3] = KNOB_PROTO_VERSION;
        reply[4] = KNOB_KEY_SLOTS;
        reply[5] = knob_engine_has_fixed_speed() ? 0x01 : 0x00; /* flags */
        break;

    case KNOB_CMD_GET_KEY: {
        const uint8_t slot = in[2];
        if (slot >= KNOB_KEY_SLOTS) {
            reply[2] = KNOB_STATUS_BAD_ARG;
            break;
        }
        reply[3] = slot;
        sys_put_le32(knob_cfg.key_codes[slot], &reply[4]);
        break;
    }

    case KNOB_CMD_SET_KEY: {
        const uint8_t slot = in[2];
        if (slot >= KNOB_KEY_SLOTS) {
            reply[2] = KNOB_STATUS_BAD_ARG;
            break;
        }
        knob_cfg.key_codes[slot] = sys_get_le32(&in[3]);
        LOG_INF("key slot %d -> %08x (staged)", slot, knob_cfg.key_codes[slot]);
        break;
    }

    case KNOB_CMD_COMMIT:
        knob_settings_save();
        LOG_INF("settings committed");
        break;

    case KNOB_CMD_RESET:
        memcpy(knob_cfg.key_codes, default_key_codes, sizeof(knob_cfg.key_codes));
        knob_cfg.pot = default_pot_cfg;
        knob_settings_save();
        LOG_INF("settings reset to defaults");
        break;

    case KNOB_CMD_GET_POT_CFG:
        reply[3] = knob_cfg.pot.role;
        reply[4] = knob_cfg.pot.speed_max_mult;
        reply[5] = knob_cfg.pot.speed_min_div;
        reply[6] = knob_cfg.pot.steps;
        break;

    case KNOB_CMD_SET_POT_CFG: {
        /* Per-role sensitivity: only the fields for the selected role are
         * updated, so switching roles keeps the other role's dial. */
        const uint8_t role = in[2];
        if (role > KNOB_POT_ROLE_OFF) {
            reply[2] = KNOB_STATUS_BAD_ARG;
            break;
        }
        knob_cfg.pot.role = role;
        /* Any valid field updates its role's dial; zeros leave it alone.
         * (Dual-pot builds tune the FIXED speed slider's range through the
         * speed fields while the settings pot runs another role.) */
        if (in[3] >= 1 && in[4] >= 1) {
            knob_cfg.pot.speed_max_mult = in[3];
            knob_cfg.pot.speed_min_div = in[4];
        }
        if (in[5] >= 2) {
            knob_cfg.pot.steps = in[5];
        }
        LOG_INF("pot cfg -> role %d, /%d..x%d, %d steps (staged)", knob_cfg.pot.role,
                knob_cfg.pot.speed_min_div, knob_cfg.pot.speed_max_mult, knob_cfg.pot.steps);
        break;
    }

    case KNOB_CMD_GET_POT_VALUE: {
        const int32_t raw = pot_raw_latest;
        if (raw < 0) {
            reply[2] = KNOB_STATUS_BAD_ARG; /* no sample yet */
            break;
        }
        sys_put_le16((uint16_t)raw, &reply[3]);
        reply[5] = knob_cfg.pot.role;
        sys_put_le16((uint16_t)pot_semantic_latest, &reply[6]);
        break;
    }

    default:
        reply[2] = KNOB_STATUS_BAD_ARG;
        break;
    }
}

/* Host->device frames arrive as SetReport(Output) on the control endpoint
 * (macOS IOHIDDeviceSetReport does this when there's no OUT interrupt EP). */
static int raw_set_report_cb(const struct device *dev, struct usb_setup_packet *setup,
                             int32_t *len, uint8_t **data) {
    if (*len < 1) {
        return 0;
    }
    uint8_t frame[RAW_REPORT_SIZE] = {0};
    memcpy(frame, *data, MIN((size_t)*len, sizeof(frame)));

    uint8_t reply[RAW_REPORT_SIZE] = {0};
    handle_frame(frame, reply);
    raw_send_reply(reply);
    return 0;
}

static int raw_get_report_cb(const struct device *dev, struct usb_setup_packet *setup,
                             int32_t *len, uint8_t **data) {
    /* Nothing meaningful to say via GetReport; replies go over the IN EP. */
    static uint8_t empty[RAW_REPORT_SIZE];
    *data = empty;
    *len = sizeof(empty);
    return 0;
}

static const struct hid_ops raw_ops = {
    .get_report = raw_get_report_cb,
    .set_report = raw_set_report_cb,
    .int_in_ready = raw_in_ready_cb,
};

static int knob_settings_init(void) {
    memcpy(knob_cfg.key_codes, default_key_codes, sizeof(knob_cfg.key_codes));
    knob_cfg.pot = default_pot_cfg;
    /* NVS overrides defaults via the settings handler during settings_load()
     * (ZMK calls it at startup for BLE bonds; our handler rides along). */

    raw_hid_dev = device_get_binding("HID_1");
    if (!raw_hid_dev) {
        LOG_ERR("HID_1 not found — set CONFIG_USB_HID_DEVICE_COUNT=2");
        return -ENODEV;
    }
    usb_hid_register_device(raw_hid_dev, raw_hid_desc, sizeof(raw_hid_desc), &raw_ops);
    int err = usb_hid_init(raw_hid_dev);
    if (err) {
        LOG_ERR("usb_hid_init(HID_1) failed (%d)", err);
        return err;
    }
    LOG_INF("raw HID settings channel up (usage page 0x%04x)", RAW_USAGE_PAGE);
    return 0;
}

/* After ZMK's own USB init (which runs at POST_KERNEL 50-ish) but before
 * usb_enable. ZMK enables USB at APPLICATION; matching hid-io's placement. */
SYS_INIT(knob_settings_init, POST_KERNEL, 91);
