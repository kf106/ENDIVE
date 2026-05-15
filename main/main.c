/*
 * USB composite: MSC (FAT on internal flash) + CDC ACM serial.
 * Based on ESP-IDF tusb_composite_msc_serialdevice example.
 *
 * TinyUSB (MSC + CDC) is not started at boot: native USB Serial/JTAG stays on /dev/ttyACM0
 * for idf.py flash/monitor. Press BOOT (GPIO0) to enumerate the composite device; the host
 * can mount the FAT volume until a session timer expires (extendable with BOOT); yellow
 * warning length and session duration come from the `config` file on the FAT volume.
 * After editing `config`, unplug and replug USB before the next BOOT (safest so the host
 * rescans the stick). If you eject the volume from the OS, the firmware ends the USB session
 * after a short debounce so BOOT works again without waiting for the full session timer.
 * The cycle repeats on the next BOOT press.
 * ESP_LOG: UART0 (TX/RX).
 *
 * LEDs (ESP32-S3 Super Mini class boards, espboards.dev style):
 * - Red “power” indicator + WS2812 RGB share GPIO48 — WS2812 is timing (GRB); red is
 *   analog from the same pin, so you cannot drive them fully independently; expect
 *   interaction/flicker if you also bit-bang GPIO48.
 * - Small blue “charge” LED is NOT on a CPU GPIO — it is the charger status (on when
 *   charging, off with battery present, blinks when no battery). Firmware cannot set it;
 *   periodic blinking is normal without a cell — it is not the MSC status indicator.
 * - WS2812: **green** = host MSC path live, **red** = not. **Blinking yellow** = last N seconds
 *   (from `config` on the volume) before the USB session auto-disconnects; press BOOT in that window
 *   (or while the host has the drive mounted) to extend the session timer. (Not the charger LED.)
 *
 * SPDX-FileCopyrightText: 2026 Keir Finlow-Bates
 *
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if CONFIG_FATFS_USE_LABEL
#include "diskio_wl.h"
#include "ff.h"
#endif
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "led_strip_types.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "tusb_msc_storage.h"

static const char *TAG = "main";

#define BASE_PATH "/usb"

/** BOOT strap / IO0 — active low on typical ESP32-S3 boards (including Super Mini class). */
#define BOOT_BUTTON_GPIO GPIO_NUM_0

/** Editable on the FAT volume at /usb/config (no firmware rebuild). */
#define BOARD_CONFIG_PATH BASE_PATH "/config"

/** Defaults and bounds after parsing `config`. */
#define BOARD_CONFIG_SESSION_DEFAULT 60
#define BOARD_CONFIG_WARNING_DEFAULT 15
#define BOARD_CONFIG_SESSION_MIN 10
#define BOARD_CONFIG_SESSION_MAX 7200
#define BOARD_CONFIG_WARNING_MIN 1
#define BOARD_CONFIG_WARNING_MAX 600

/**
 * After the host has exposed MSC (FAT handed to USB), if the medium returns to the
 * firmware (e.g. OS eject / USB reconfigure) for this long, we tear down TinyUSB like a
 * session timeout so BOOT works again without a cable replug.
 */
#define HOST_RELEASE_DEBOUNCE_MS 1000

/** FAT volume label for MSC (host desktop name; max 11 chars for classic FAT). */
#define MSC_FAT_VOLUME_LABEL "ENDIVE"

/** WS2812 data (same pad as red power leg on many Super Minis — see file header). */
#define STATUS_WS2812_GPIO 48

/** Half-period (ms) for pre-unmount yellow: LED alternates yellow ↔ off each interval. */
#define WARNING_BLINK_HALF_PERIOD_MS 250

static uint8_t s_rx_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];

static QueueHandle_t s_app_queue;

typedef struct {
    uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];
    size_t buf_len;
    uint8_t itf;
} app_message_t;

void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    (void)event;
    size_t rx_size = 0;
    esp_err_t ret = tinyusb_cdcacm_read(itf, s_rx_buf, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &rx_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CDC read error");
        return;
    }
    app_message_t tx_msg = {
        .buf_len = rx_size,
        .itf = (uint8_t)itf,
    };
    memcpy(tx_msg.buf, s_rx_buf, rx_size);
    xQueueSend(s_app_queue, &tx_msg, 0);
}

void tinyusb_cdc_line_state_changed_callback(int itf, cdcacm_event_t *event)
{
    int dtr = event->line_state_changed_data.dtr;
    int rts = event->line_state_changed_data.rts;
    ESP_LOGI(TAG, "CDC line state ch.%d: DTR:%d RTS:%d", itf, dtr, rts);
}

static bool file_exists(const char *file_path)
{
    struct stat buffer;
    return stat(file_path, &buffer) == 0;
}

/** Text files placed on the FAT volume (visible on the PC as the USB disk). */
static void write_file_if_missing(const char *path, const char *contents)
{
    if (file_exists(path)) {
        return;
    }
    ESP_LOGI(TAG, "Creating %s", path);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Could not create %s: %s", path, strerror(errno));
        return;
    }
    if (fputs(contents, f) < 0) {
        ESP_LOGE(TAG, "Write failed: %s", path);
    }
    fclose(f);
}

static void trim_line(char *s)
{
    char *t = s;
    while (*t == ' ' || *t == '\t' || *t == '\r' || *t == '\n') {
        t++;
    }
    if (t != s) {
        memmove(s, t, strlen(t) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
}

/** Session length (s) and yellow phase (s); written atomically after parse+clamp. */
static volatile uint32_t s_cfg_session_s = BOARD_CONFIG_SESSION_DEFAULT;
static volatile uint32_t s_cfg_warning_s = BOARD_CONFIG_WARNING_DEFAULT;

static void board_config_clamp(uint32_t *session_s, uint32_t *warn_s)
{
    if (*session_s < BOARD_CONFIG_SESSION_MIN) {
        *session_s = BOARD_CONFIG_SESSION_MIN;
    }
    if (*session_s > BOARD_CONFIG_SESSION_MAX) {
        *session_s = BOARD_CONFIG_SESSION_MAX;
    }
    if (*warn_s < BOARD_CONFIG_WARNING_MIN) {
        *warn_s = BOARD_CONFIG_WARNING_MIN;
    }
    if (*warn_s > BOARD_CONFIG_WARNING_MAX) {
        *warn_s = BOARD_CONFIG_WARNING_MAX;
    }
    if (*warn_s >= *session_s) {
        *warn_s = (*session_s > 1) ? (*session_s - 1) : 1;
    }
}

static void board_config_load(void)
{
    uint32_t session_s = BOARD_CONFIG_SESSION_DEFAULT;
    uint32_t warn_s = BOARD_CONFIG_WARNING_DEFAULT;

    FILE *f = fopen(BOARD_CONFIG_PATH, "r");
    if (f == NULL) {
        ESP_LOGW(TAG, "Could not open %s (%s) — using defaults session=%" PRIu32 " warning=%" PRIu32,
                 BOARD_CONFIG_PATH, strerror(errno), session_s, warn_s);
        s_cfg_session_s = session_s;
        s_cfg_warning_s = warn_s;
        return;
    }

    char line[192];
    while (fgets(line, sizeof(line), f) != NULL) {
        trim_line(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        char *eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        *eq++ = '\0';
        trim_line(line);
        trim_line(eq);
        char *endp = NULL;
        unsigned long v = strtoul(eq, &endp, 10);
        if (endp == eq) {
            continue;
        }
        if (strcmp(line, "session_seconds") == 0) {
            session_s = (uint32_t)v;
        } else if (strcmp(line, "warning_seconds") == 0) {
            warn_s = (uint32_t)v;
        }
    }
    fclose(f);

    board_config_clamp(&session_s, &warn_s);
    s_cfg_session_s = session_s;
    s_cfg_warning_s = warn_s;
    ESP_LOGI(TAG, "Loaded %s: session_seconds=%" PRIu32 " warning_seconds=%" PRIu32,
             BOARD_CONFIG_PATH, session_s, warn_s);
}

static void ensure_board_config_file(void)
{
    static const char k_config[] =
        "# USB session timing (seconds). Edit while this volume is mounted from your PC.\n"
        "#\n"
        "# After you change this file: unplug USB from the PC and plug back in before the next\n"
        "# BOOT so the host rescans reliably. (Volume eject alone usually auto-ends the session.)\n"
        "#\n"
        "# Values are read at boot and whenever a USB session ends (session timer or host\n"
        "# eject) and the firmware remounts this FAT partition.\n"
        "#\n"
        "# session_seconds — MSC+CDC stays up this long before auto-disconnect (10–7200).\n"
        "# warning_seconds — NeoPixel yellow for this many seconds before disconnect;\n"
        "#                   must be less than session_seconds (1–600).\n"
        "\n"
        "session_seconds=60\n"
        "warning_seconds=15\n";

    write_file_if_missing(BOARD_CONFIG_PATH, k_config);
}

/**
 * Host-side helper: run on a Linux PC with bash after the drive is mounted, e.g.
 *   bash /media/you/VOLUME/link-home-env.sh
 * Makes ~/.env a symlink to the .env file stored on this USB volume.
 */
static void boot_button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
}

/**
 * Wait for a debounced BOOT press (active low), then release so we do not re-trigger
 * on the same press.
 */
static void wait_boot_press_debounced(void)
{
    const int poll_ms = 20;
    const int hold_ms = 50;
    int low_ms = 0;

    for (;;) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            low_ms += poll_ms;
            if (low_ms >= hold_ms) {
                while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(poll_ms));
                }
                vTaskDelay(pdMS_TO_TICKS(50));
                return;
            }
        } else {
            low_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
}

static int s_boot_ext_low_ms;
static bool s_boot_ext_armed;

static void boot_reset_extend_detector(void)
{
    s_boot_ext_low_ms = 0;
    s_boot_ext_armed = false;
}

/** Call every ~20ms while a USB session is active; true once per BOOT hold (≥50ms) + release. */
static bool boot_poll_session_extend_edge(void)
{
    const int poll_ms = 20;

    if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
        s_boot_ext_low_ms += poll_ms;
        if (s_boot_ext_low_ms >= 50) {
            s_boot_ext_armed = true;
        }
    } else {
        if (s_boot_ext_armed) {
            s_boot_ext_armed = false;
            s_boot_ext_low_ms = 0;
            return true;
        }
        s_boot_ext_low_ms = 0;
    }
    return false;
}

/** TinyUSB MSC+CDC session is running; `host_msc_led_task` uses this + deadline for yellow. */
static volatile bool s_usb_session_live;

/** FreeRTOS tick when the session will be torn down unless extended. */
static volatile TickType_t s_session_end_tick;

static bool session_extend_allowed(TickType_t now, TickType_t deadline)
{
    TickType_t rem = deadline - now;
    const uint32_t warn_s = s_cfg_warning_s;
    const TickType_t warn_ticks = pdMS_TO_TICKS(warn_s * 1000U);
    const bool in_warning = rem <= warn_ticks;
    const bool host_mounted = tinyusb_msc_storage_in_use_by_usb_host();
    (void)now;
    return in_warning || host_mounted;
}

static void usb_msc_cdc_session_task(void *arg)
{
    (void)arg;
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = NULL,
        .string_descriptor = NULL,
        .string_descriptor_count = 0,
        .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = NULL,
        .hs_configuration_descriptor = NULL,
        .qualifier_descriptor = NULL,
#else
        .configuration_descriptor = NULL,
#endif
    };
    const tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = 64,
        .callback_rx = &tinyusb_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };

    for (;;) {
        const uint32_t sess_s = s_cfg_session_s;
        const uint32_t warn_s = s_cfg_warning_s;
        ESP_LOGI(TAG, "MSC+CDC idle — press BOOT (GPIO%d) to expose USB drive (session %" PRIu32 "s, yellow last %" PRIu32 "s — see %s).",
                 (int)BOOT_BUTTON_GPIO, sess_s, warn_s, BOARD_CONFIG_PATH);
        wait_boot_press_debounced();

        ESP_LOGI(TAG, "TinyUSB composite (MSC + CDC) starting…");
        ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
        ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm_cfg));
        ESP_ERROR_CHECK(tinyusb_cdcacm_register_callback(
            TINYUSB_CDC_ACM_0, CDC_EVENT_LINE_STATE_CHANGED, &tinyusb_cdc_line_state_changed_callback));

        boot_reset_extend_detector();

        const uint32_t session_s = s_cfg_session_s;
        const uint32_t warning_s = s_cfg_warning_s;
        ESP_LOGI(TAG, "USB up: host may mount FAT + use CDC. Session up to %" PRIu32 "s (yellow last %" PRIu32 "s; BOOT extends if mounted or in yellow).",
                 session_s, warning_s);

        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(session_s * 1000U);
        s_session_end_tick = deadline;
        s_usb_session_live = true;

        const TickType_t poll_ticks = pdMS_TO_TICKS(20);
        bool seen_host_expose_msc = false;
        TickType_t host_hidden_ticks = 0;
        bool ended_after_host_release = false;

        while (xTaskGetTickCount() < deadline) {
            vTaskDelay(poll_ticks);
            TickType_t now = xTaskGetTickCount();

            /* `in_use_by_usb_host` == !is_fat_mounted: false after tud_umount_cb remounts FAT
             * (typical OS eject / configuration drop) while TinyUSB is still installed. */
            const bool host_exposes_msc = tinyusb_msc_storage_in_use_by_usb_host();
            if (host_exposes_msc) {
                seen_host_expose_msc = true;
                host_hidden_ticks = 0;
            } else if (seen_host_expose_msc) {
                host_hidden_ticks += poll_ticks;
                if (host_hidden_ticks >= pdMS_TO_TICKS(HOST_RELEASE_DEBOUNCE_MS)) {
                    ended_after_host_release = true;
                    ESP_LOGI(TAG, "Host released MSC (eject / USB reconfigure) — ending USB session.");
                    break;
                }
            }

            if (boot_poll_session_extend_edge()) {
                if (now >= deadline) {
                    break;
                }
                if (session_extend_allowed(now, deadline)) {
                    const uint32_t extend_s = s_cfg_session_s;
                    deadline = now + pdMS_TO_TICKS(extend_s * 1000U);
                    s_session_end_tick = deadline;
                    host_hidden_ticks = 0;
                    ESP_LOGI(TAG, "Session extended: full %" PRIu32 "s from now.", extend_s);
                }
            }
        }

        s_usb_session_live = false;

        if (ended_after_host_release) {
            ESP_LOGI(TAG, "Disconnecting USB stack after host release.");
        } else {
            ESP_LOGW(TAG, "Session timeout — disconnecting USB (eject on host if still mounted).");
        }
        ESP_ERROR_CHECK(tusb_cdc_acm_deinit(TINYUSB_CDC_ACM_0));
        ESP_ERROR_CHECK(tinyusb_driver_uninstall());

        /* Reclaim FAT for firmware; matches tud_umount_cb default path when base_path was NULL. */
        vTaskDelay(pdMS_TO_TICKS(150));
        ESP_ERROR_CHECK(tinyusb_msc_storage_mount(BASE_PATH));
        board_config_load();
        ESP_LOGI(TAG, "FAT remounted at %s — ready for next BOOT press.", BASE_PATH);
    }
}

static void ensure_host_env_symlink_kit(void)
{
    static const char k_dotenv[] =
        "# Real environment variables live here on the USB drive.\n"
        "# Edit this file, then (re)run:  bash link-home-env.sh  on your PC if needed.\n"
        "# Example:\n"
        "# MY_TOKEN=replace-me\n";

    static const char k_script[] =
        "#!/usr/bin/env bash\n"
        "# Run from this directory on your Linux PC while the USB volume is mounted:\n"
        "#   bash link-home-env.sh\n"
        "# FAT does not store the Unix execute bit — chmod +x and ./link-home-env.sh will fail.\n"
        "# Creates ~/.env -> <this drive>/.env\n"
        "set -euo pipefail\n"
        "HERE=\"$(cd \"$(dirname \"${BASH_SOURCE[0]}\")\" && pwd)\"\n"
        "TARGET=\"$HERE/.env\"\n"
        "if [[ ! -f \"$TARGET\" ]]; then\n"
        "  echo \"Expected .env next to this script: $TARGET\" >&2\n"
        "  exit 1\n"
        "fi\n"
        "ln -sf \"$TARGET\" \"$HOME/.env\"\n"
        "echo \"OK: $HOME/.env -> $TARGET\"\n";

    static const char k_readme[] =
        "USB volume from ESP32-S3 Super Mini firmware\n"
        "\n"
        "1. Plug the board into your PC, then press BOOT (GPIO0) to expose the removable FAT volume as \"" MSC_FAT_VOLUME_LABEL "\" (eject/replug if an old name is cached).\n"
        "2. Edit `config` on this volume to change USB session length and yellow warning time (seconds).\n"
        "   After saving `config`, unplug the board's USB from the PC and plug it back in before\n"
        "   the next BOOT so the host picks up changes reliably.\n"
        "   (If you only eject the volume, the device usually ends the USB session automatically\n"
        "   within about a second so you can press BOOT again without replugging.)\n"
        "3. Edit .env here if you want (that file is the source of truth).\n"
        "4. In a terminal, cd to this mount point and run:\n"
        "     bash link-home-env.sh\n"
        "   (Do not use ./link-home-env.sh after chmod +x — FAT cannot preserve execute bits.)\n"
        "   That makes a symlink:  ~/.env  ->  this drive's .env\n"
        "\n"
        "Eject the drive from the PC before unplugging.\n";

    write_file_if_missing(BASE_PATH "/.env", k_dotenv);
    write_file_if_missing(BASE_PATH "/link-home-env.sh", k_script);
    write_file_if_missing(BASE_PATH "/README-ENV-LINK.txt", k_readme);
    ensure_board_config_file();
}

static esp_err_t storage_init_spiflash(wl_handle_t *wl_handle)
{
    ESP_LOGI(TAG, "Wear levelling on FAT partition…");
    const esp_partition_t *data_partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    if (data_partition == NULL) {
        ESP_LOGE(TAG, "No DATA/FAT partition (check partitions.csv).");
        return ESP_ERR_NOT_FOUND;
    }
    return wl_mount(data_partition, wl_handle);
}

#if CONFIG_FATFS_USE_LABEL
static void set_fat_volume_label(wl_handle_t wl_handle)
{
    BYTE pdrv = ff_diskio_get_pdrv_wl(wl_handle);
    if (pdrv == 0xff) {
        ESP_LOGW(TAG, "f_setlabel skipped: WL drive not registered yet");
        return;
    }
    char spec[20];
    snprintf(spec, sizeof(spec), "%u:" MSC_FAT_VOLUME_LABEL, (unsigned)pdrv);
    FRESULT fr = f_setlabel(spec);
    if (fr != FR_OK) {
        ESP_LOGW(TAG, "f_setlabel(%s) failed: %d", spec, (int)fr);
    } else {
        ESP_LOGI(TAG, "FAT volume label set to " MSC_FAT_VOLUME_LABEL " (replug/eject to refresh on host)");
    }
}
#endif

/**
 * When this returns true, the FAT volume is no longer mounted inside the firmware and is
 * handed to USB MSC — that is when Linux can (and usually will) mount the filesystem. It
 * goes false again after host eject / disconnect (see TinyUSB tud_umount_cb / SCSI paths).
 */
static led_strip_handle_t s_status_led = NULL;

static void status_ws2812_set_msc_for_host(bool host_live);

static esp_err_t status_ws2812_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = STATUS_WS2812_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        /* Super Mini onboard NeoPixel is usually GRB order (see ESPHome/board docs). */
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_status_led);
    if (err != ESP_OK) {
        return err;
    }
    /* Red = MSC not yet handed to host (same meaning once host ejects). */
    status_ws2812_set_msc_for_host(false);
    return ESP_OK;
}

static void status_ws2812_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_status_led == NULL) {
        return;
    }
    esp_err_t err = led_strip_set_pixel(s_status_led, 0, r, g, b);
    if (err == ESP_OK) {
        (void)led_strip_refresh(s_status_led);
    }
}

static void status_ws2812_set_msc_for_host(bool host_live)
{
    /* Logical RGB; strip is configured GRB for this board. */
    if (host_live) {
        status_ws2812_set_rgb(0, 140, 0);
    } else {
        status_ws2812_set_rgb(140, 0, 0);
    }
}

static void host_msc_led_task(void *arg)
{
    (void)arg;
    /* Debounce: MSC "in use" can flicker during SCSI traffic; require stable samples. */
    const int k_stable_ticks = 5;
    bool pending = false;
    int pending_ticks = 0;
    bool applied = false;
    bool prev_warn = false;
    /* Yellow blink: drive phase from elapsed time vs anchor — not (now/half)&1, which beats
     * against the ~120 ms poll and looks random when samples miss phase boundaries. */
    static TickType_t s_warn_blink_anchor;
    static bool s_warn_blink_show_yellow;

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        const bool session = s_usb_session_live;
        const TickType_t end = s_session_end_tick;
        const uint32_t warn_s = s_cfg_warning_s;
        const TickType_t warn_ticks = pdMS_TO_TICKS(warn_s * 1000U);
        const bool warn = session && now < end && (end - now) <= warn_ticks;

        if (warn != prev_warn) {
            if (warn) {
                s_warn_blink_anchor = now;
                s_warn_blink_show_yellow = true;
            }
            prev_warn = warn;
            ESP_LOGI(TAG, "Session pre-unmount hint: %s (NeoPixel %s)", warn ? "on" : "off", warn ? "blinking yellow" : "normal");
        }

        if (!session) {
            /* Session over — debounce can still say "host live" briefly; never show stale green. */
            status_ws2812_set_msc_for_host(false);
            pending = false;
            pending_ticks = 0;
            applied = false;
        } else if (now >= end) {
            /* Past deadline but TinyUSB not torn down yet — same as idle (no green flash). */
            status_ws2812_set_msc_for_host(false);
            pending = false;
            pending_ticks = 0;
            applied = false;
        } else if (warn) {
            const TickType_t half = pdMS_TO_TICKS(WARNING_BLINK_HALF_PERIOD_MS);
            if (half > 0) {
                while ((TickType_t)(now - s_warn_blink_anchor) >= half) {
                    s_warn_blink_anchor += half;
                    s_warn_blink_show_yellow = !s_warn_blink_show_yellow;
                }
            }
            if (s_warn_blink_show_yellow) {
                status_ws2812_set_rgb(110, 90, 0);
            } else {
                status_ws2812_set_rgb(0, 0, 0);
            }
        } else {
            const bool raw = tinyusb_msc_storage_in_use_by_usb_host();
            if (raw == pending) {
                if (pending_ticks < k_stable_ticks) {
                    pending_ticks++;
                }
            } else {
                pending = raw;
                pending_ticks = 0;
            }
            if (pending_ticks >= k_stable_ticks) {
                if (pending != applied) {
                    applied = pending;
                    ESP_LOGI(TAG, "MSC for host (debounced): %s — NeoPixel GPIO%d %s",
                             applied ? "live" : "idle", STATUS_WS2812_GPIO,
                             applied ? "(green)" : "(red)");
                }
                status_ws2812_set_msc_for_host(applied);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(120));
    }
}

void app_main(void)
{
    s_app_queue = xQueueCreate(5, sizeof(app_message_t));
    assert(s_app_queue);

    ESP_LOGI(TAG, "Init MSC storage (SPI flash + WL)…");

    static wl_handle_t wl_handle = WL_INVALID_HANDLE;
    ESP_ERROR_CHECK(storage_init_spiflash(&wl_handle));

    const tinyusb_msc_spiflash_config_t config_spi = {.wl_handle = wl_handle};
    ESP_ERROR_CHECK(tinyusb_msc_storage_init_spiflash(&config_spi));
    ESP_ERROR_CHECK(tinyusb_msc_storage_mount(BASE_PATH));

#if CONFIG_FATFS_USE_LABEL
    set_fat_volume_label(wl_handle);
#endif

    ensure_host_env_symlink_kit();
    board_config_load();

    boot_button_init();

    if (status_ws2812_init() == ESP_OK) {
        xTaskCreate(host_msc_led_task, "msc_led", 4096, NULL, 3, NULL);
    } else {
        ESP_LOGW(TAG, "WS2812 on GPIO%d not initialized — host-mount LED disabled", STATUS_WS2812_GPIO);
    }

    xTaskCreate(usb_msc_cdc_session_task, "usb_sess", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "Ready: UART log here; /dev/ttyACM0 = USB-Serial-JTAG until BOOT starts TinyUSB.");

    app_message_t msg;
    for (;;) {
        if (xQueueReceive(s_app_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg.buf_len == 0) {
            continue;
        }
        ESP_LOGD(TAG, "CDC ch.%u len=%u", (unsigned)msg.itf, (unsigned)msg.buf_len);
        ESP_LOG_BUFFER_HEXDUMP(TAG, msg.buf, msg.buf_len, ESP_LOG_INFO);
        ESP_ERROR_CHECK(tinyusb_cdcacm_write_queue(msg.itf, msg.buf, msg.buf_len));
        esp_err_t err = tinyusb_cdcacm_write_flush(msg.itf, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "CDC flush: %s", esp_err_to_name(err));
        }
    }
}
