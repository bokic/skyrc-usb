/**
 * skyrc-usb - command-line interface for libskyrc-usb
 *
 * Subcommands:
 *   scan / list                     Scan and enumerate connected SkyRC USB chargers.
 *   info        [device]            Print charger model, hardware/firmware version, serial.
 *   status      [device] [--channel C]
 *                                   Print current program state and cell voltages.
 *   system      [device] [--channel C]
 *                                   Print system configuration and protection limits.
 *   monitor     [device] [--channel C] [--interval MS]
 *                                   Continuously poll and print live telemetry until Ctrl-C.
 *   start       [device] [--channel C] --battery B --mode M --cells N
 *                        --charge-current A [--discharge-current A] [options...]
 *   stop        [device] [--channel C]
 *   set-speaker [device] [--channel C] --key on|off --buzzer on|off
 *   set-capacity [device] [--channel C] on|off [--limit mAh]
 *   set-time    [device] [--channel C] on|off [--minutes N]
 *   set-temp    [device] [--channel C] on|off [--threshold C]
 *   set-rest-time [device] [--channel C] --minutes N
 *   list-ops    [model]             List battery chemistries and modes.
 *   raw         [device] <hex-bytes>
 *                                   Send raw frame and print response.
 *   help                            Show this help.
 */

#include "skyrc-usb.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;

static void handle_sigint(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ========================================================================== */
/* Option Parsing Helpers                                                     */
/* ========================================================================== */

static const char *find_opt(int argc, char **argv, const char *key)
{
    for (int i = 0; i < argc - 1; ++i) {
        if (strcmp(argv[i], key) == 0) return argv[i + 1];
    }
    return NULL;
}

static bool has_flag(int argc, char **argv, const char *flag)
{
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], flag) == 0) return true;
    }
    return false;
}

static bool parse_uint8(const char *s, uint8_t *out)
{
    if (!s || *s == '-') return false;
    char *end = NULL;
    errno = 0;
    unsigned long val = strtoul(s, &end, 10);
    if (errno || end == s || *end || val > 255) return false;
    *out = (uint8_t)val;
    return true;
}

static bool parse_uint16(const char *s, uint16_t *out)
{
    if (!s || *s == '-') return false;
    char *end = NULL;
    errno = 0;
    unsigned long val = strtoul(s, &end, 10);
    if (errno || end == s || *end || val > 65535) return false;
    *out = (uint16_t)val;
    return true;
}

static bool parse_float(const char *s, float *out)
{
    if (!s) return false;
    char *end = NULL;
    errno = 0;
    float val = strtof(s, &end);
    if (errno || end == s || *end) return false;
    *out = val;
    return true;
}

static bool parse_on_off(const char *s, bool *out)
{
    if (!s) return false;
    if (strcasecmp(s, "on") == 0 || strcasecmp(s, "1") == 0 || strcasecmp(s, "true") == 0) {
        *out = true;
        return true;
    }
    if (strcasecmp(s, "off") == 0 || strcasecmp(s, "0") == 0 || strcasecmp(s, "false") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool is_on_off_str(const char *s)
{
    bool dummy = false;
    return parse_on_off(s, &dummy);
}

static bool is_number_str(const char *s)
{
    if (!s || !*s) return false;
    for (const char *p = s; *p; ++p) {
        if (!isdigit((unsigned char)*p)) return false;
    }
    return true;
}

static int parse_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex(const char *text, uint8_t **bytes, size_t *length)
{
    size_t count = 0;
    for (const char *p = text; *p; ++p) {
        if (*p != ':' && *p != ' ' && *p != '-' && *p != ',') ++count;
    }
    if (count == 0 || count % 2 != 0) return false;
    uint8_t *result = malloc(count / 2);
    if (!result) return false;
    int high = -1;
    size_t out = 0;
    for (const char *p = text; *p; ++p) {
        if (*p == ':' || *p == ' ' || *p == '-' || *p == ',') continue;
        int nibble = parse_hex_nibble(*p);
        if (nibble < 0) { free(result); return false; }
        if (high < 0) {
            high = nibble;
        } else {
            result[out++] = (uint8_t)((high << 4) | nibble);
            high = -1;
        }
    }
    *bytes = result;
    *length = out;
    return true;
}

struct index_search_ctx {
    int target_idx;
    int cur_idx;
    char *found_path;
};

static void index_search_cb(void *context, const skyrc_usb_scan_result *result)
{
    struct index_search_ctx *ctx = context;
    ctx->cur_idx++;
    if (ctx->cur_idx == ctx->target_idx && !ctx->found_path && skyrc_usb_scan_result_get_path(result)) {
        ctx->found_path = strdup(skyrc_usb_scan_result_get_path(result));
    }
}

static char *resolve_device_target(const char *arg)
{
    if (!arg || !*arg || *arg == '-') return NULL;
    char *end = NULL;
    long idx = strtol(arg, &end, 10);
    if (end != arg && *end == '\0' && idx > 0) {
        struct index_search_ctx ctx = { .target_idx = (int)idx, .cur_idx = 0, .found_path = NULL };
        skyrc_usb_scan(0, 0, index_search_cb, &ctx);
        if (ctx.found_path) return ctx.found_path;
    }
    return strdup(arg);
}

static skyrc_usb_device *open_device_arg(const char *arg)
{
    skyrc_usb_device *dev = NULL;
    enum skyrc_usb_error err;

    char *target_path = resolve_device_target(arg);
    if (target_path) {
        err = skyrc_usb_open_path(target_path, &dev);
        if (err != SKYRC_USB_OK) {
            fprintf(stderr, "Error: Could not open device '%s': %s\n", target_path, skyrc_usb_error_string(err));
            if (err == SKYRC_USB_PERMISSION_DENIED) {
                fprintf(stderr, "\nTip: USB device permissions required. To grant access without root:\n"
                                "     echo 'SUBSYSTEM==\"hidraw\", ATTRS{idVendor}==\"0000\", ATTRS{idProduct}==\"0001\", MODE=\"0666\"' | sudo tee /etc/udev/rules.d/99-skyrc.rules\n"
                                "     sudo udevadm trigger\n"
                                "     Or run the command with sudo.\n");
            }
            free(target_path);
            return NULL;
        }
        free(target_path);
        return dev;
    }

    err = skyrc_usb_open_default(&dev);
    if (err != SKYRC_USB_OK) {
        fprintf(stderr, "Error: Could not open SkyRC USB charger: %s\n", skyrc_usb_error_string(err));
        if (err == SKYRC_USB_PERMISSION_DENIED) {
            fprintf(stderr, "\nTip: USB device permissions required. To grant access without root:\n"
                            "     echo 'SUBSYSTEM==\"hidraw\", ATTRS{idVendor}==\"0000\", ATTRS{idProduct}==\"0001\", MODE=\"0666\"' | sudo tee /etc/udev/rules.d/99-skyrc.rules\n"
                            "     sudo udevadm trigger\n"
                            "     Or run the command with sudo.\n");
        }
        return NULL;
    }
    return dev;
}

/* ========================================================================== */
/* Commands                                                                   */
/* ========================================================================== */

static void print_scan_device(void *context, const skyrc_usb_scan_result *result)
{
    int *count = context;
    (*count)++;

    printf("[%d] Path: %s\n", *count, skyrc_usb_scan_result_get_path(result));
    printf("    VID:PID:        %04x:%04x\n",
           skyrc_usb_scan_result_get_vendor_id(result),
           skyrc_usb_scan_result_get_product_id(result));

    const char *prod = skyrc_usb_scan_result_get_product_string(result);
    if (prod && *prod) printf("    Product:        %s\n", prod);

    const char *mfg = skyrc_usb_scan_result_get_manufacturer_string(result);
    if (mfg && *mfg) printf("    Manufacturer:   %s\n", mfg);

    const char *serial = skyrc_usb_scan_result_get_serial_number(result);
    if (serial && *serial) printf("    Serial:         %s\n", serial);
    printf("\n");
}

static int cmd_scan(int argc, char **argv)
{
    uint16_t vid = SKYRC_USB_DEFAULT_VID;
    uint16_t pid = SKYRC_USB_DEFAULT_PID;

    const char *vid_str = find_opt(argc, argv, "--vid");
    if (vid_str) vid = (uint16_t)strtoul(vid_str, NULL, 16);

    const char *pid_str = find_opt(argc, argv, "--pid");
    if (pid_str) pid = (uint16_t)strtoul(pid_str, NULL, 16);

    if (has_flag(argc, argv, "--all")) {
        vid = 0;
        pid = 0;
    }

    printf("Scanning for SkyRC USB devices...\n\n");
    int count = 0;
    enum skyrc_usb_error err = skyrc_usb_scan(vid, pid, print_scan_device, &count);
    if (err != SKYRC_USB_OK) {
        fprintf(stderr, "Scan failed: %s\n", skyrc_usb_error_string(err));
        return 1;
    }

    if (count == 0) {
        printf("No SkyRC chargers detected.\n");
    } else {
        printf("Found %d device(s).\n", count);
    }
    return 0;
}

static int cmd_info(int argc, char **argv)
{
    const char *target = (argc > 2 && argv[2][0] != '-') ? argv[2] : NULL;
    skyrc_usb_device *dev = open_device_arg(target);
    if (!dev) return 1;

    skyrc_usb_device_info *info = skyrc_usb_device_info_create();
    if (!info) {
        skyrc_usb_close(dev);
        return 1;
    }

    enum skyrc_usb_error err = skyrc_usb_get_device_info(dev, info);
    if (err != SKYRC_USB_OK) {
        fprintf(stderr, "Failed to get device info: %s\n", skyrc_usb_error_string(err));
        skyrc_usb_device_info_free(info);
        skyrc_usb_close(dev);
        return 1;
    }

    printf("Charger Information:\n");
    printf("  Model Name:       %s\n", skyrc_usb_device_info_get_model_name(info));
    printf("  Core Type:        %s\n", skyrc_usb_device_info_get_core_type(info));
    printf("  Machine ID:       %s\n", skyrc_usb_device_info_get_machine_id(info));
    printf("  Firmware Version: %.2f\n", (double)skyrc_usb_device_info_get_firmware_version(info));
    printf("  Hardware Version: %u\n", skyrc_usb_device_info_get_hardware_version(info));
    printf("  Customer ID:      %u\n", skyrc_usb_device_info_get_customer_id(info));
    printf("  Language ID:      %u\n", skyrc_usb_device_info_get_language_id(info));
    printf("  Encrypted:        %s\n", skyrc_usb_device_info_is_encrypted(info) ? "yes" : "no");

    skyrc_usb_device_info_free(info);
    skyrc_usb_close(dev);
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    const char *target = (argc > 2 && argv[2][0] != '-') ? argv[2] : NULL;
    uint8_t channel = 0;
    const char *ch_str = find_opt(argc, argv, "--channel");
    if (ch_str) parse_uint8(ch_str, &channel);

    skyrc_usb_device *dev = open_device_arg(target);
    if (!dev) return 1;

    skyrc_usb_current_state *state = skyrc_usb_current_state_create();
    if (!state) {
        skyrc_usb_close(dev);
        return 1;
    }

    enum skyrc_usb_error err = skyrc_usb_get_current_state(dev, channel, state);
    if (err != SKYRC_USB_OK) {
        fprintf(stderr, "Failed to get status: %s\n", skyrc_usb_error_string(err));
        skyrc_usb_current_state_free(state);
        skyrc_usb_close(dev);
        return 1;
    }

    printf("Channel %u Current State:\n", channel + 1);
    printf("  Work Mode:            %u\n", skyrc_usb_current_state_get_work_mode(state));
    printf("  Max Charge Current:   %.2f A\n",
           (double)skyrc_usb_current_state_get_max_charge_current(state) / 1000.0);
    printf("  Max Discharge Current: %.2f A\n",
           (double)skyrc_usb_current_state_get_max_discharge_current(state) / 1000.0);

    printf("  Cell Voltages:\n");
    for (size_t i = 0; i < SKYRC_USB_MAX_CELLS; ++i) {
        uint16_t mv = 0;
        if (skyrc_usb_current_state_get_cell(state, i, &mv) && mv > 0) {
            printf("    Cell %zu: %.3f V (%u mV)\n", i + 1, (double)mv / 1000.0, mv);
        }
    }

    skyrc_usb_current_state_free(state);
    skyrc_usb_close(dev);
    return 0;
}

static int cmd_system(int argc, char **argv)
{
    const char *target = (argc > 2 && argv[2][0] != '-') ? argv[2] : NULL;
    uint8_t channel = 0;
    const char *ch_str = find_opt(argc, argv, "--channel");
    if (ch_str) parse_uint8(ch_str, &channel);

    skyrc_usb_device *dev = open_device_arg(target);
    if (!dev) return 1;

    skyrc_usb_system_info *sys = skyrc_usb_system_info_create();
    if (!sys) {
        skyrc_usb_close(dev);
        return 1;
    }

    enum skyrc_usb_error err = skyrc_usb_get_system_info(dev, channel, sys);
    if (err != SKYRC_USB_OK) {
        fprintf(stderr, "Failed to get system configuration: %s\n", skyrc_usb_error_string(err));
        skyrc_usb_system_info_free(sys);
        skyrc_usb_close(dev);
        return 1;
    }

    printf("Channel %u System Settings:\n", channel + 1);
    printf("  Key Beep:             %s\n", skyrc_usb_system_info_get_key_beep(sys) ? "enabled" : "disabled");
    printf("  Buzzer Beep:          %s\n", skyrc_usb_system_info_get_system_beep(sys) ? "enabled" : "disabled");
    printf("  Capacity Protection:  %s (%u mAh)\n",
           skyrc_usb_system_info_get_capacity_enabled(sys) ? "enabled" : "disabled",
           skyrc_usb_system_info_get_capacity_mah(sys));
    printf("  Time Protection:      %s (%u min)\n",
           skyrc_usb_system_info_get_time_protection_enabled(sys) ? "enabled" : "disabled",
           skyrc_usb_system_info_get_time_protection_minutes(sys));
    printf("  Temp Protection:      %s (%u °C)\n",
           skyrc_usb_system_info_get_temp_protection_enabled(sys) ? "enabled" : "disabled",
           skyrc_usb_system_info_get_temp_protection_threshold(sys));
    printf("  Rest Time:            %u min\n", skyrc_usb_system_info_get_rest_time_minutes(sys));
    printf("  DC Input Low Cut:     %u V\n", skyrc_usb_system_info_get_dc_input_low(sys));
    printf("  NiMH Delta Peak:      %u mV/cell\n", skyrc_usb_system_info_get_nimh_peak(sys));
    printf("  NiCd Delta Peak:      %u mV/cell\n", skyrc_usb_system_info_get_nicd_peak(sys));
    if (skyrc_usb_system_info_get_ac_power(sys) > 0) {
        printf("  AC Power Limit:       %u W\n", skyrc_usb_system_info_get_ac_power(sys));
    }

    skyrc_usb_system_info_free(sys);
    skyrc_usb_close(dev);
    return 0;
}

static int cmd_monitor(int argc, char **argv)
{
    const char *target = (argc > 2 && argv[2][0] != '-') ? argv[2] : NULL;
    uint8_t channel = 0;
    const char *ch_str = find_opt(argc, argv, "--channel");
    if (ch_str) parse_uint8(ch_str, &channel);

    uint16_t interval_ms = 1000;
    const char *int_str = find_opt(argc, argv, "--interval");
    if (int_str) parse_uint16(int_str, &interval_ms);
    if (interval_ms < 100) interval_ms = 100;

    skyrc_usb_device *dev = open_device_arg(target);
    if (!dev) return 1;

    skyrc_usb_real_data *data = skyrc_usb_real_data_create();
    if (!data) {
        skyrc_usb_close(dev);
        return 1;
    }

    signal(SIGINT, handle_sigint);
    printf("Monitoring Channel %u live telemetry (polling every %u ms, Ctrl-C to stop)...\n\n",
           channel + 1, interval_ms);

    while (g_running) {
        enum skyrc_usb_error err = skyrc_usb_get_real_data(dev, channel, data);
        if (err == SKYRC_USB_OK) {
            uint16_t t = skyrc_usb_real_data_get_time(data);
            unsigned int mins = t / 60;
            unsigned int secs = t % 60;

            float v = skyrc_usb_real_data_get_voltage(data);
            float a = skyrc_usb_real_data_get_current(data);
            float w = v * a;

            printf("[%02u:%02u] Mode:%u %6.2fV %5.2fA %6.1fW %5umAh %2u°C",
                   mins, secs,
                   skyrc_usb_real_data_get_work_mode(data),
                   (double)v, (double)a, (double)w,
                   skyrc_usb_real_data_get_capacity(data),
                   skyrc_usb_real_data_get_ext_temp(data));

            uint16_t ir = skyrc_usb_real_data_get_internal_resistance(data);
            if (ir > 0) printf(" IR:%umΩ", ir);

            size_t cell_cnt = skyrc_usb_real_data_get_cell_count(data);
            if (cell_cnt > 0) {
                printf(" Cells: [");
                for (size_t i = 0; i < cell_cnt; ++i) {
                    uint16_t cv = 0;
                    skyrc_usb_real_data_get_cell(data, i, &cv);
                    printf("%s%.3f", (i > 0) ? " " : "", (double)cv / 1000.0);
                }
                printf("]V");
            }

            const char *err_str = skyrc_usb_real_data_get_error_string(data);
            if (err_str) printf(" ERR: %s", err_str);
            printf("\n");
            fflush(stdout);
        } else {
            fprintf(stderr, "Telemetry read error: %s\n", skyrc_usb_error_string(err));
        }

        usleep((useconds_t)interval_ms * 1000u);
    }

    printf("\nMonitoring stopped.\n");
    skyrc_usb_real_data_free(data);
    skyrc_usb_close(dev);
    return 0;
}

static int cmd_start(int argc, char **argv)
{
    const char *target = (argc > 2 && argv[2][0] != '-') ? argv[2] : NULL;
    uint8_t channel = 0;
    const char *ch_str = find_opt(argc, argv, "--channel");
    if (ch_str) parse_uint8(ch_str, &channel);

    const char *batt_str = find_opt(argc, argv, "--battery");
    const char *mode_str = find_opt(argc, argv, "--mode");
    const char *cells_str = find_opt(argc, argv, "--cells");
    const char *cur_str = find_opt(argc, argv, "--charge-current");

    if (!batt_str || !mode_str || !cells_str || !cur_str) {
        fprintf(stderr, "Error: Missing required parameters: --battery, --mode, --cells, --charge-current\n");
        return 1;
    }

    skyrc_usb_charge_params p;
    memset(&p, 0, sizeof(p));

    /* Battery type parsing */
    if (strcasecmp(batt_str, "lipo") == 0) p.battery_type = 0;
    else if (strcasecmp(batt_str, "liio") == 0) p.battery_type = 1;
    else if (strcasecmp(batt_str, "life") == 0) p.battery_type = 2;
    else if (strcasecmp(batt_str, "lihv") == 0) p.battery_type = 3;
    else if (strcasecmp(batt_str, "nimh") == 0) p.battery_type = 4;
    else if (strcasecmp(batt_str, "nicd") == 0) p.battery_type = 5;
    else if (strcasecmp(batt_str, "pb")   == 0) p.battery_type = 6;
    else if (strcasecmp(batt_str, "smart") == 0) p.battery_type = 7;
    else parse_uint8(batt_str, &p.battery_type);

    /* Mode parsing */
    if (strcasecmp(mode_str, "charge") == 0) p.mode = 0;
    else if (strcasecmp(mode_str, "discharge") == 0) p.mode = 1;
    else if (strcasecmp(mode_str, "storage") == 0) p.mode = 2;
    else if (strcasecmp(mode_str, "fast") == 0 || strcasecmp(mode_str, "fast charge") == 0) p.mode = 3;
    else if (strcasecmp(mode_str, "balance") == 0 || strcasecmp(mode_str, "balance charge") == 0) p.mode = 4;
    else if (strcasecmp(mode_str, "auto") == 0 || strcasecmp(mode_str, "auto charge") == 0) p.mode = 1;
    else if (strcasecmp(mode_str, "repeak") == 0 || strcasecmp(mode_str, "re-peak") == 0) p.mode = 3;
    else if (strcasecmp(mode_str, "cycle") == 0) p.mode = 4;
    else parse_uint8(mode_str, &p.mode);

    if (!parse_uint8(cells_str, &p.cell_count) || p.cell_count == 0) {
        fprintf(stderr, "Error: Invalid cell count '%s'\n", cells_str);
        return 1;
    }

    float chg_cur = 0.0f;
    if (!parse_float(cur_str, &chg_cur) || chg_cur <= 0.0f) {
        fprintf(stderr, "Error: Invalid charge current '%s'\n", cur_str);
        return 1;
    }
    p.charge_current_ma = (uint16_t)(chg_cur * 1000.0f);

    const char *dcur_str = find_opt(argc, argv, "--discharge-current");
    if (dcur_str) {
        float dcur = 0.0f;
        if (parse_float(dcur_str, &dcur) && dcur > 0.0f) {
            p.discharge_current_ma = (uint16_t)(dcur * 1000.0f);
        }
    }

    skyrc_usb_device *dev = open_device_arg(target);
    if (!dev) return 1;

    printf("Starting operation on Channel %u: Battery=%s Mode=%s Cells=%u Current=%.2fA...\n",
           channel + 1, batt_str, mode_str, p.cell_count, (double)chg_cur);

    enum skyrc_usb_error err = skyrc_usb_start(dev, channel, &p);
    if (err != SKYRC_USB_OK) {
        fprintf(stderr, "Start failed: %s\n", skyrc_usb_error_string(err));
        skyrc_usb_close(dev);
        return 1;
    }

    printf("Start command sent successfully.\n");
    skyrc_usb_close(dev);
    return 0;
}

static int cmd_stop(int argc, char **argv)
{
    const char *target = (argc > 2 && argv[2][0] != '-') ? argv[2] : NULL;
    uint8_t channel = 0;
    const char *ch_str = find_opt(argc, argv, "--channel");
    if (ch_str) parse_uint8(ch_str, &channel);

    skyrc_usb_device *dev = open_device_arg(target);
    if (!dev) return 1;

    printf("Stopping operation on Channel %u...\n", channel + 1);
    enum skyrc_usb_error err = skyrc_usb_stop(dev, channel);
    if (err != SKYRC_USB_OK) {
        fprintf(stderr, "Stop failed: %s\n", skyrc_usb_error_string(err));
        skyrc_usb_close(dev);
        return 1;
    }

    printf("Stop command sent successfully.\n");
    skyrc_usb_close(dev);
    return 0;
}

static int cmd_set_speaker(int argc, char **argv)
{
    const char *target = (argc > 2 && argv[2][0] != '-') ? argv[2] : NULL;
    uint8_t channel = 0;
    const char *ch_str = find_opt(argc, argv, "--channel");
    if (ch_str) parse_uint8(ch_str, &channel);

    const char *key_str = find_opt(argc, argv, "--key");
    const char *buzz_str = find_opt(argc, argv, "--buzzer");

    if (!key_str || !buzz_str) {
        fprintf(stderr, "Error: Must specify --key on|off and --buzzer on|off\n");
        return 1;
    }

    bool key_on = false;
    bool buzz_on = false;
    if (!parse_on_off(key_str, &key_on) || !parse_on_off(buzz_str, &buzz_on)) {
        fprintf(stderr, "Error: Invalid on/off value for --key or --buzzer\n");
        return 1;
    }

    skyrc_usb_device *dev = open_device_arg(target);
    if (!dev) return 1;

    enum skyrc_usb_error err = skyrc_usb_set_speaker(dev, channel, key_on, buzz_on);
    if (err != SKYRC_USB_OK) {
        fprintf(stderr, "Failed to set speaker: %s\n", skyrc_usb_error_string(err));
        skyrc_usb_close(dev);
        return 1;
    }

    printf("Speaker settings updated: Key=%s Buzzer=%s\n",
           key_on ? "on" : "off", buzz_on ? "on" : "off");
    skyrc_usb_close(dev);
    return 0;
}

static int cmd_set_capacity(int argc, char **argv)
{
    const char *target = (argc > 2 && argv[2][0] != '-' && !is_on_off_str(argv[2])) ? argv[2] : NULL;
    uint8_t channel = 0;
    const char *ch_str = find_opt(argc, argv, "--channel");
    if (ch_str) parse_uint8(ch_str, &channel);

    bool enabled = false;
    uint16_t limit = 5000;

    for (int i = 2; i < argc; ++i) {
        if (parse_on_off(argv[i], &enabled)) break;
    }

    const char *lim_str = find_opt(argc, argv, "--limit");
    if (lim_str) parse_uint16(lim_str, &limit);

    skyrc_usb_device *dev = open_device_arg(target);
    if (!dev) return 1;

    enum skyrc_usb_error err = skyrc_usb_set_capacity_protection(dev, channel, enabled, limit);
    if (err != SKYRC_USB_OK) {
        fprintf(stderr, "Failed to set capacity protection: %s\n", skyrc_usb_error_string(err));
        skyrc_usb_close(dev);
        return 1;
    }

    printf("Capacity protection updated: %s (%u mAh)\n", enabled ? "enabled" : "disabled", limit);
    skyrc_usb_close(dev);
    return 0;
}

static int cmd_set_time(int argc, char **argv)
{
    const char *target = (argc > 2 && argv[2][0] != '-' && !is_on_off_str(argv[2])) ? argv[2] : NULL;
    uint8_t channel = 0;
    const char *ch_str = find_opt(argc, argv, "--channel");
    if (ch_str) parse_uint8(ch_str, &channel);

    bool enabled = false;
    uint16_t mins = 120;

    for (int i = 2; i < argc; ++i) {
        if (parse_on_off(argv[i], &enabled)) break;
    }

    const char *m_str = find_opt(argc, argv, "--minutes");
    if (m_str) parse_uint16(m_str, &mins);

    skyrc_usb_device *dev = open_device_arg(target);
    if (!dev) return 1;

    enum skyrc_usb_error err = skyrc_usb_set_time_protection(dev, channel, enabled, mins);
    if (err != SKYRC_USB_OK) {
        fprintf(stderr, "Failed to set time protection: %s\n", skyrc_usb_error_string(err));
        skyrc_usb_close(dev);
        return 1;
    }

    printf("Time protection updated: %s (%u minutes)\n", enabled ? "enabled" : "disabled", mins);
    skyrc_usb_close(dev);
    return 0;
}

static int cmd_set_temp(int argc, char **argv)
{
    const char *target = (argc > 2 && argv[2][0] != '-' && !is_number_str(argv[2])) ? argv[2] : NULL;
    uint8_t channel = 0;
    const char *ch_str = find_opt(argc, argv, "--channel");
    if (ch_str) parse_uint8(ch_str, &channel);

    uint8_t threshold = 50;
    if (argc > 2 && is_number_str(argv[2])) parse_uint8(argv[2], &threshold);
    const char *t_str = find_opt(argc, argv, "--threshold");
    if (t_str) parse_uint8(t_str, &threshold);

    skyrc_usb_device *dev = open_device_arg(target);
    if (!dev) return 1;

    enum skyrc_usb_error err = skyrc_usb_set_temp_protection(dev, channel, true, threshold);
    if (err != SKYRC_USB_OK) {
        fprintf(stderr, "Failed to set temperature protection: %s\n", skyrc_usb_error_string(err));
        skyrc_usb_close(dev);
        return 1;
    }

    printf("Temperature protection updated: threshold=%u °C\n", threshold);
    skyrc_usb_close(dev);
    return 0;
}

static int cmd_set_rest_time(int argc, char **argv)
{
    const char *target = (argc > 2 && argv[2][0] != '-' && !is_number_str(argv[2])) ? argv[2] : NULL;
    uint8_t channel = 0;
    const char *ch_str = find_opt(argc, argv, "--channel");
    if (ch_str) parse_uint8(ch_str, &channel);

    uint8_t mins = 10;
    if (argc > 2 && is_number_str(argv[2])) parse_uint8(argv[2], &mins);
    const char *m_str = find_opt(argc, argv, "--minutes");
    if (m_str) parse_uint8(m_str, &mins);

    skyrc_usb_device *dev = open_device_arg(target);
    if (!dev) return 1;

    enum skyrc_usb_error err = skyrc_usb_set_rest_time(dev, channel, mins);
    if (err != SKYRC_USB_OK) {
        fprintf(stderr, "Failed to set rest time: %s\n", skyrc_usb_error_string(err));
        skyrc_usb_close(dev);
        return 1;
    }

    printf("Rest time updated: %u minutes\n", mins);
    skyrc_usb_close(dev);
    return 0;
}

static int cmd_list_ops(int argc, char **argv)
{
    const char *model = (argc > 2) ? argv[2] : NULL;
    const skyrc_usb_operations *ops = skyrc_usb_get_operations(model);
    if (!ops) {
        fprintf(stderr, "No operations found for model '%s'\n", model ? model : "default");
        return 1;
    }

    size_t b_count = skyrc_usb_operations_get_battery_type_count(ops);
    printf("Supported battery types and modes:\n");
    for (size_t b = 0; b < b_count; ++b) {
        const char *bname = skyrc_usb_operations_get_battery_type(ops, b);
        printf("\n  [%zu] %s:\n", b, bname);
        size_t m_count = skyrc_usb_operations_get_mode_count(ops, b);
        for (size_t m = 0; m < m_count; ++m) {
            const char *mname = skyrc_usb_operations_get_mode(ops, b, m);
            printf("      [%zu] %s\n", m, mname);
        }
    }
    printf("\n");
    return 0;
}

static int cmd_raw(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: skyrc-usb raw [device] <hex-bytes>\n");
        return 1;
    }

    const char *hex_str = argv[argc - 1];
    const char *target = (argc > 3) ? argv[2] : NULL;

    uint8_t *bytes = NULL;
    size_t len = 0;
    if (!parse_hex(hex_str, &bytes, &len) || len == 0) {
        fprintf(stderr, "Error: Invalid hex string '%s'\n", hex_str);
        return 1;
    }

    skyrc_usb_device *dev = open_device_arg(target);
    if (!dev) {
        free(bytes);
        return 1;
    }

    uint8_t cmd = bytes[0];
    uint8_t channel = (len > 1) ? bytes[1] : 0;
    const uint8_t *payload = (len > 2) ? &bytes[2] : NULL;
    size_t payload_len = (len > 2) ? (len - 2) : 0;

    printf("Sending frame (cmd=0x%02x, ch=%u, payload_len=%zu)...\n", cmd, channel, payload_len);
    enum skyrc_usb_error err = skyrc_usb_send_frame(dev, cmd, channel, payload, payload_len);
    free(bytes);

    if (err != SKYRC_USB_OK) {
        fprintf(stderr, "Send failed: %s\n", skyrc_usb_error_string(err));
        skyrc_usb_close(dev);
        return 1;
    }

    uint8_t rcmd = 0;
    uint8_t rch = 0;
    uint8_t resp[SKYRC_USB_REPORT_SIZE];
    size_t rlen = 0;

    err = skyrc_usb_receive_frame(dev, &rcmd, &rch, resp, &rlen, sizeof(resp), 1000);
    if (err != SKYRC_USB_OK) {
        fprintf(stderr, "Receive failed: %s\n", skyrc_usb_error_string(err));
        skyrc_usb_close(dev);
        return 1;
    }

    printf("Received: cmd=0x%02x ch=%u len=%zu: ", rcmd, rch, rlen);
    for (size_t i = 0; i < rlen; ++i) {
        printf("%02x", resp[i]);
    }
    printf("\n");

    skyrc_usb_close(dev);
    return 0;
}

static void print_usage(FILE *stream)
{
    fprintf(stream,
        "Usage: skyrc-usb <command> [arguments]\n\n"
        "Commands:\n"
        "  scan / list [--vid V] [--pid P] [--all]       Enumerate connected SkyRC USB devices\n"
        "  info / version [device]                       Print charger identity and firmware/hardware version\n"
        "  status      [device] [--channel C]            Print current program status and cell voltages\n"
        "  system      [device] [--channel C]            Print system settings and cutoffs\n"
        "  monitor     [device] [--channel C] [--interval MS]  Stream live measurements until Ctrl-C\n"
        "  start       [device] [--channel C] --battery B --mode M --cells N --charge-current A [options...]\n"
        "  stop        [device] [--channel C]            Stop charging on specified channel\n"
        "  set-speaker [device] [--channel C] --key on|off --buzzer on|off\n"
        "  set-capacity [device] [--channel C] on|off [--limit mAh]\n"
        "  set-time    [device] [--channel C] on|off [--minutes N]\n"
        "  set-temp    [device] [--channel C] on|off [--threshold C]\n"
        "  set-rest-time [device] [--channel C] --minutes N\n"
        "  list-ops    [model]                           List supported chemistries and modes\n"
        "  raw         [device] <hex-bytes>              Send raw frame and print reply\n"
        "  help                                          Show this help\n\n"
        "Channel is 0-indexed (0=Channel 1/A, 1=Channel 2/B, etc.). Default is 0.\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(stderr);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "scan") == 0 || strcmp(cmd, "list") == 0) {
        return cmd_scan(argc, argv);
    }
    if (strcmp(cmd, "info") == 0 || strcmp(cmd, "version") == 0 || strcmp(cmd, "fw-version") == 0) {
        return cmd_info(argc, argv);
    }
    if (strcmp(cmd, "status") == 0) {
        return cmd_status(argc, argv);
    }
    if (strcmp(cmd, "system") == 0) {
        return cmd_system(argc, argv);
    }
    if (strcmp(cmd, "monitor") == 0) {
        return cmd_monitor(argc, argv);
    }
    if (strcmp(cmd, "start") == 0) {
        return cmd_start(argc, argv);
    }
    if (strcmp(cmd, "stop") == 0) {
        return cmd_stop(argc, argv);
    }
    if (strcmp(cmd, "set-speaker") == 0) {
        return cmd_set_speaker(argc, argv);
    }
    if (strcmp(cmd, "set-capacity") == 0) {
        return cmd_set_capacity(argc, argv);
    }
    if (strcmp(cmd, "set-time") == 0) {
        return cmd_set_time(argc, argv);
    }
    if (strcmp(cmd, "set-temp") == 0) {
        return cmd_set_temp(argc, argv);
    }
    if (strcmp(cmd, "set-rest-time") == 0) {
        return cmd_set_rest_time(argc, argv);
    }
    if (strcmp(cmd, "list-ops") == 0) {
        return cmd_list_ops(argc, argv);
    }
    if (strcmp(cmd, "raw") == 0) {
        return cmd_raw(argc, argv);
    }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_usage(stdout);
        return 0;
    }

    fprintf(stderr, "Unknown command '%s'. Run 'skyrc-usb help' for usage.\n", cmd);
    return 1;
}
