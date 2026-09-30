#include "skyrc-usb.h"

#include <hidapi.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>

/* ========================================================================== */
/* Error Strings                                                              */
/* ========================================================================== */

const char *skyrc_usb_error_string(enum skyrc_usb_error error)
{
    switch (error) {
    case SKYRC_USB_OK:                  return "success";
    case SKYRC_USB_INVALID_ARGUMENT:    return "invalid argument";
    case SKYRC_USB_DEVICE_NOT_FOUND:    return "device not found";
    case SKYRC_USB_PERMISSION_DENIED:   return "permission denied (run with sudo or configure udev rules)";
    case SKYRC_USB_OPEN_FAILED:         return "failed to open USB HID device";
    case SKYRC_USB_WRITE_FAILED:        return "USB write failed";
    case SKYRC_USB_READ_FAILED:         return "USB read failed";
    case SKYRC_USB_TIMEOUT:             return "operation timed out";
    case SKYRC_USB_CHECKSUM_ERROR:      return "packet checksum error";
    case SKYRC_USB_INVALID_PACKET:      return "invalid or corrupt packet";
    case SKYRC_USB_UNSUPPORTED_COMMAND: return "unsupported command";
    case SKYRC_USB_OUT_OF_MEMORY:       return "out of memory";
    }
    return "unknown error";
}

/* ========================================================================== */
/* HIDAPI Lifecycle Management                                                */
/* ========================================================================== */

static int g_hidapi_refcount = 0;

static bool skyrc_hid_init(void)
{
    if (g_hidapi_refcount++ == 0) {
        if (hid_init() != 0) {
            g_hidapi_refcount--;
            return false;
        }
    }
    return true;
}

static void skyrc_hid_exit(void)
{
    if (g_hidapi_refcount > 0 && --g_hidapi_refcount == 0) {
        hid_exit();
    }
}

/* ========================================================================== */
/* Data Structures                                                            */
/* ========================================================================== */

struct skyrc_usb_scan_result {
    char *path;
    uint16_t vendor_id;
    uint16_t product_id;
    char *serial_number;
    char *product_string;
    char *manufacturer_string;
    int release_number;
    int interface_number;
};

struct skyrc_usb_device {
    hid_device *handle;
    char *path;
    uint16_t vendor_id;
    uint16_t product_id;
    skyrc_usb_device_info *cached_info;
};

struct skyrc_usb_device_info {
    char core_type[7];
    char model_name[32];
    char machine_id[33];
    float software_version;
    uint8_t hardware_version;
    uint16_t customer_id;
    uint8_t language_id;
    uint8_t upgrade_type;
    bool is_encrypt;
};

struct skyrc_usb_real_data {
    uint8_t work_mode;
    bool is_working;
    bool is_finished;
    uint16_t time_seconds;
    float voltage;
    float current;
    uint16_t capacity_mah;
    uint8_t ext_temp;
    uint8_t int_temp;
    uint16_t internal_resistance_mohm;
    size_t cell_count;
    uint16_t cells_mv[SKYRC_USB_MAX_CELLS];
    uint16_t error_code;
    const char *error_string;
};

struct skyrc_usb_current_state {
    uint8_t work_mode;
    uint16_t max_charge_current_ma;
    uint16_t max_discharge_current_ma;
    uint16_t cells_mv[SKYRC_USB_MAX_CELLS];
};

struct skyrc_usb_system_info {
    bool key_beep;
    bool system_beep;
    bool capacity_enabled;
    uint16_t capacity_mah;
    bool time_protection_enabled;
    uint16_t time_protection_minutes;
    bool temp_protection_enabled;
    uint8_t temp_threshold_c;
    uint8_t rest_time_minutes;
    uint8_t dc_input_low;
    uint8_t nimh_peak_mv;
    uint8_t nicd_peak_mv;
    uint8_t ac_power_w;
};

struct skyrc_usb_operations {
    const char *const *battery_types;
    size_t battery_type_count;
    const char *const *const *modes;
};

/* ========================================================================== */
/* Model Core ID Lookup Table                                                 */
/* ========================================================================== */

struct skyrc_core_map {
    const char *core_type;
    const char *model_name;
};

static const struct skyrc_core_map SKYRC_CORE_TABLE[] = {
    {"100064", "B6"},
    {"6x80+",  "6x80+"},
    {"100084", "B6 Mini"},
    {"100008", "B6AC V2"},
    {"100087", "Ultimate Duo 1400W"},
    {"100089", "D100"},
    {"100097", "D200"},
    {"100104", "Q200"},
    {"100123", "D400"},
    {"100125", "D250"},
    {"100131", "D100 V2"},
    {"100157", "D260"},
    {"100158", "Q200 V2"},
    {"100160", "B6AC+"},
    {"100173", "PC1500"},
    {"100182", "T1000 Maestro"},
    {"100185", "NC2500 Pro"},
    {"100188", "PC3000H"},
    {"100196", "D200neo"},
    {"100197", "Q200neo"},
    {"100198", "B6neo"},
    {"100199", "D100neo"},
    {"100200", "B6ACneo"},
    {"100201", "D750 MIX"},
    {"100202", "S100neo"},
    {"100203", "D200neo"},
    {"100204", "ZX-3000H"},
    {"100208", "P2X"},
    {"100211", "B6neo+"},
    {"100212", "M15-1200W"},
    {"100213", "GMA620MP"},
    {"100216", "JY-PC3000W"},
    {"100220", "PC1080neo"},
    {"100221", "PC3020-1"},
    {"100222", "YZ-114PLUS"},
    {"100223", "Q100neo"},
    {"100225", "B6neo 2"},
    {"100226", "PC1500neo"},
    {"100227", "PC3020-2"},
    {"600153", "BD380"},
    {"200029", "PSU2000"},
    {"100231", "PC7200"},
    {"100236", "CH003"},
    {"100237", "CH005"},
    {"100239", "CH007"},
    {NULL, NULL}
};

const char *skyrc_usb_lookup_model_name(const char *core_type)
{
    if (!core_type) return "Unknown";
    for (size_t i = 0; SKYRC_CORE_TABLE[i].core_type != NULL; ++i) {
        if (strncmp(core_type, SKYRC_CORE_TABLE[i].core_type, 6) == 0) {
            return SKYRC_CORE_TABLE[i].model_name;
        }
    }
    return "Unknown SkyRC";
}

/* ========================================================================== */
/* Error Strings Table (from Charger Master)                                 */
/* ========================================================================== */

static const char *const SKYRC_ERROR_STRINGS[32] = {
    "Int.temp too high!",
    "Ext.temp too high!",
    "DC in too low!",
    "DC in too high!",
    "Over TIME LIMIT!",
    "Capacity Limit",
    "REVERSE POLARITY!",
    "Control Failed!",
    "Break Down!",
    "Input Failed!",
    "Connection Break!",
    "Cell Error! Voltage invalid!",
    "BALANCE CONNECTION ERROR!",
    "Battery Not Connected!",
    "Cell Number Incorrect!",
    "Connection Error! Check main port!",
    "Fully Charged!",
    "Not need charge!",
    "Cell error! High voltage!",
    "Connection Break!",
    "Connection Break!",
    "Connection Break!",
    "Battery error!",
    "Balance Volt. Difference Too High",
    "Only One Mode Can Be Active At A Time",
    "Output Overload!",
    "DC Mode Error!",
    "AC-DC Too Low!",
    "DC Power Setting Error!",
    "Discharger: No Current",
    "Discharger: Offline",
    "Other Error"
};

/* ========================================================================== */
/* Wide Char to UTF-8 Helper                                                 */
/* ========================================================================== */

static char *wchar_to_utf8(const wchar_t *wstr)
{
    if (!wstr) return NULL;
    size_t len = wcstombs(NULL, wstr, 0);
    if (len == (size_t)-1) return NULL;
    char *res = malloc(len + 1);
    if (!res) return NULL;
    wcstombs(res, wstr, len + 1);
    return res;
}

/* ========================================================================== */
/* Device Enumeration Implementation                                          */
/* ========================================================================== */

const char *skyrc_usb_scan_result_get_path(const skyrc_usb_scan_result *result)
{
    return result ? result->path : NULL;
}

uint16_t skyrc_usb_scan_result_get_vendor_id(const skyrc_usb_scan_result *result)
{
    return result ? result->vendor_id : 0;
}

uint16_t skyrc_usb_scan_result_get_product_id(const skyrc_usb_scan_result *result)
{
    return result ? result->product_id : 0;
}

const char *skyrc_usb_scan_result_get_serial_number(const skyrc_usb_scan_result *result)
{
    return result ? result->serial_number : NULL;
}

const char *skyrc_usb_scan_result_get_manufacturer_string(const skyrc_usb_scan_result *result)
{
    return result ? result->manufacturer_string : NULL;
}

const char *skyrc_usb_scan_result_get_product_string(const skyrc_usb_scan_result *result)
{
    return result ? result->product_string : NULL;
}

enum skyrc_usb_error skyrc_usb_scan(uint16_t vendor_id, uint16_t product_id,
                                    skyrc_usb_scan_callback callback, void *context)
{
    if (!callback) return SKYRC_USB_INVALID_ARGUMENT;
    if (!skyrc_hid_init()) return SKYRC_USB_OPEN_FAILED;

    /* Enumerate using specified VID/PID or scan all if 0 */
    struct hid_device_info *devs = hid_enumerate(vendor_id, product_id);
    for (struct hid_device_info *cur = devs; cur != NULL; cur = cur->next) {
        /* If scanning with 0, filter only plausible devices (VID 0x0000:0x0001 or matching manufacturer) */
        if (vendor_id == 0 && product_id == 0) {
            bool matches = (cur->vendor_id == SKYRC_USB_DEFAULT_VID &&
                            cur->product_id == SKYRC_USB_DEFAULT_PID);
            if (!matches && cur->product_string) {
                char *prod = wchar_to_utf8(cur->product_string);
                if (prod) {
                    if (strcasestr(prod, "skyrc") || strcasestr(prod, "charger")) {
                        matches = true;
                    }
                    free(prod);
                }
            }
            if (!matches && cur->manufacturer_string) {
                char *mfg = wchar_to_utf8(cur->manufacturer_string);
                if (mfg) {
                    if (strcasestr(mfg, "skyrc")) {
                        matches = true;
                    }
                    free(mfg);
                }
            }
            if (!matches) continue;
        }

        skyrc_usb_scan_result res;
        memset(&res, 0, sizeof(res));
        res.path = cur->path ? strdup(cur->path) : NULL;
        res.vendor_id = cur->vendor_id;
        res.product_id = cur->product_id;
        res.serial_number = wchar_to_utf8(cur->serial_number);
        res.product_string = wchar_to_utf8(cur->product_string);
        res.manufacturer_string = wchar_to_utf8(cur->manufacturer_string);
        res.release_number = cur->release_number;
        res.interface_number = cur->interface_number;

        callback(context, &res);

        free(res.path);
        free(res.serial_number);
        free(res.product_string);
        free(res.manufacturer_string);
    }

    hid_free_enumeration(devs);
    skyrc_hid_exit();
    return SKYRC_USB_OK;
}

/* ========================================================================== */
/* Connection Management Implementation                                       */
/* ========================================================================== */

static skyrc_usb_device *skyrc_usb_device_alloc(hid_device *handle, const char *path,
                                               uint16_t vid, uint16_t pid)
{
    skyrc_usb_device *dev = calloc(1, sizeof(*dev));
    if (!dev) return NULL;
    dev->handle = handle;
    dev->path = path ? strdup(path) : NULL;
    dev->vendor_id = vid;
    dev->product_id = pid;
    return dev;
}

enum skyrc_usb_error skyrc_usb_open_path(const char *path, skyrc_usb_device **device)
{
    if (!path || !device) return SKYRC_USB_INVALID_ARGUMENT;
    *device = NULL;

    if (!skyrc_hid_init()) return SKYRC_USB_OPEN_FAILED;

    errno = 0;
    hid_device *h = hid_open_path(path);
    if (!h) {
        int err_no = errno;
        skyrc_hid_exit();
        if (err_no == EACCES || err_no == EPERM) {
            return SKYRC_USB_PERMISSION_DENIED;
        }
        return SKYRC_USB_OPEN_FAILED;
    }

    *device = skyrc_usb_device_alloc(h, path, 0, 0);
    if (!*device) {
        hid_close(h);
        skyrc_hid_exit();
        return SKYRC_USB_OUT_OF_MEMORY;
    }

    return SKYRC_USB_OK;
}

enum skyrc_usb_error skyrc_usb_open(uint16_t vendor_id, uint16_t product_id,
                                    const char *serial, skyrc_usb_device **device)
{
    if (!device) return SKYRC_USB_INVALID_ARGUMENT;
    *device = NULL;

    if (!skyrc_hid_init()) return SKYRC_USB_OPEN_FAILED;

    wchar_t *wserial = NULL;
    if (serial) {
        size_t len = strlen(serial);
        wserial = malloc((len + 1) * sizeof(wchar_t));
        if (wserial) {
            mbstowcs(wserial, serial, len + 1);
        }
    }

    errno = 0;
    hid_device *h = hid_open(vendor_id, product_id, wserial);
    free(wserial);

    if (!h) {
        int err_no = errno;
        skyrc_hid_exit();
        if (err_no == EACCES || err_no == EPERM) {
            return SKYRC_USB_PERMISSION_DENIED;
        }
        return SKYRC_USB_DEVICE_NOT_FOUND;
    }

    *device = skyrc_usb_device_alloc(h, NULL, vendor_id, product_id);
    if (!*device) {
        hid_close(h);
        skyrc_hid_exit();
        return SKYRC_USB_OUT_OF_MEMORY;
    }

    return SKYRC_USB_OK;
}

struct default_search_ctx {
    char *found_path;
};

static void default_search_cb(void *context, const skyrc_usb_scan_result *result)
{
    struct default_search_ctx *ctx = context;
    if (!ctx->found_path && result->path) {
        ctx->found_path = strdup(result->path);
    }
}

enum skyrc_usb_error skyrc_usb_open_default(skyrc_usb_device **device)
{
    if (!device) return SKYRC_USB_INVALID_ARGUMENT;

    /* First attempt direct open of VID 0x0000 PID 0x0001 */
    enum skyrc_usb_error err = skyrc_usb_open(SKYRC_USB_DEFAULT_VID, SKYRC_USB_DEFAULT_PID, NULL, device);
    if (err == SKYRC_USB_OK) return SKYRC_USB_OK;
    if (err == SKYRC_USB_PERMISSION_DENIED) return SKYRC_USB_PERMISSION_DENIED;

    /* If direct open fails, enumerate devices to find matching path */
    struct default_search_ctx ctx = { .found_path = NULL };
    skyrc_usb_scan(SKYRC_USB_DEFAULT_VID, SKYRC_USB_DEFAULT_PID, default_search_cb, &ctx);
    if (!ctx.found_path) {
        skyrc_usb_scan(0, 0, default_search_cb, &ctx);
    }

    if (ctx.found_path) {
        err = skyrc_usb_open_path(ctx.found_path, device);
        free(ctx.found_path);
        return err;
    }

    return SKYRC_USB_DEVICE_NOT_FOUND;
}

void skyrc_usb_close(skyrc_usb_device *device)
{
    if (!device) return;
    if (device->handle) {
        hid_close(device->handle);
        device->handle = NULL;
    }
    if (device->cached_info) {
        skyrc_usb_device_info_free(device->cached_info);
        device->cached_info = NULL;
    }
    free(device->path);
    free(device);
    skyrc_hid_exit();
}

/* ========================================================================== */
/* Frame Transmission & Reception                                             */
/* ========================================================================== */

enum skyrc_usb_error skyrc_usb_send_frame(skyrc_usb_device *device,
                                          uint8_t cmd, uint8_t channel,
                                          const uint8_t *payload, size_t payload_len)
{
    if (!device || !device->handle) return SKYRC_USB_INVALID_ARGUMENT;
    /* 64-byte report minus header (3) and tail (3) leaves up to 58 bytes payload */
    if (payload_len > 58) return SKYRC_USB_INVALID_ARGUMENT;

    uint8_t buffer[SKYRC_USB_REPORT_SIZE + 1];
    memset(buffer, 0, sizeof(buffer));

    /* buffer[0] = Report ID (0 for unnumbered reports) */
    buffer[0] = 0x00;
    buffer[1] = 0x0F; /* UART_DATA_START */

    /* Length field: bytes from index 3 up to and including checksum */
    /* payload_len + 1 (cmd) + 1 (slot) + 1 (checksum) = payload_len + 3 */
    uint8_t frame_len = (uint8_t)(payload_len + 3);
    buffer[2] = frame_len;
    buffer[3] = cmd;
    buffer[4] = (channel < 4) ? (uint8_t)(1u << channel) : channel;

    if (payload && payload_len > 0) {
        memcpy(&buffer[5], payload, payload_len);
    }

    /* Compute additive 8-bit checksum over cmd, channel, and payload */
    uint8_t chk = 0;
    size_t chk_end = 5 + payload_len;
    for (size_t i = 3; i < chk_end; ++i) {
        chk = (uint8_t)(chk + buffer[i]);
    }

    buffer[chk_end] = chk;
    buffer[chk_end + 1] = 0xFF;
    buffer[chk_end + 2] = 0xFF;

    int written = hid_write(device->handle, buffer, sizeof(buffer));
    if (written < 0) {
        return SKYRC_USB_WRITE_FAILED;
    }

    return SKYRC_USB_OK;
}

enum skyrc_usb_error skyrc_usb_receive_frame(skyrc_usb_device *device,
                                             uint8_t *cmd, uint8_t *channel,
                                             uint8_t *payload, size_t *payload_len,
                                             size_t max_payload_len, int timeout_ms)
{
    if (!device || !device->handle) return SKYRC_USB_INVALID_ARGUMENT;

    uint8_t raw[SKYRC_USB_REPORT_SIZE + 1];
    memset(raw, 0, sizeof(raw));

    int res = hid_read_timeout(device->handle, raw, sizeof(raw), timeout_ms);
    if (res < 0) return SKYRC_USB_READ_FAILED;
    if (res == 0) return SKYRC_USB_TIMEOUT;

    const uint8_t *p = raw;
    size_t len = (size_t)res;

    /* Some systems return Report ID as first byte */
    if (len > 0 && p[0] == 0x00) {
        p++;
        len--;
    }

    if (len < 3) return SKYRC_USB_INVALID_PACKET;

    /* Check if packet has 0x0F header */
    if (p[0] == 0x0F) {
        uint8_t flen = p[1];
        if (flen < 3 || (size_t)flen + 2 > len) return SKYRC_USB_INVALID_PACKET;

        uint8_t r_cmd = p[2];
        uint8_t r_mask = p[3];

        /* Verify checksum: sum from p[2] to p[flen] inclusive */
        uint8_t sum = 0;
        for (size_t i = 2; i <= flen; ++i) {
            sum = (uint8_t)(sum + p[i]);
        }
        uint8_t expected_chk = p[flen + 1];
        if (sum != expected_chk) {
            return SKYRC_USB_CHECKSUM_ERROR;
        }

        if (cmd) *cmd = r_cmd;
        if (channel) {
            /* Decode slot mask (e.g. 1<<0 => 0, 1<<1 => 1, etc.) */
            uint8_t ch = 0;
            for (uint8_t i = 0; i < 4; ++i) {
                if (r_mask & (uint8_t)(1u << i)) {
                    ch = i;
                    break;
                }
            }
            *channel = ch;
        }

        size_t data_len = (flen >= 3) ? (size_t)(flen - 3) : 0;
        if (payload_len) *payload_len = data_len;
        if (payload && data_len > 0) {
            size_t copy_len = (data_len < max_payload_len) ? data_len : max_payload_len;
            memcpy(payload, &p[4], copy_len);
        }
        return SKYRC_USB_OK;
    }

    /* Direct / Unframed packet: starts with CMD (e.g. 0x55, 0x57, 0x5A, 0x5F, 0xF0) */
    uint8_t r_cmd = p[0];
    uint8_t r_ch = p[1];
    if (cmd) *cmd = r_cmd;
    if (channel) *channel = r_ch;

    size_t data_len = (len >= 2) ? (len - 2) : 0;
    if (payload_len) *payload_len = data_len;
    if (payload && data_len > 0) {
        size_t copy_len = (data_len < max_payload_len) ? data_len : max_payload_len;
        memcpy(payload, &p[2], copy_len);
    }

    return SKYRC_USB_OK;
}

enum skyrc_usb_error skyrc_usb_transceive(skyrc_usb_device *device,
                                          uint8_t send_cmd, uint8_t channel,
                                          const uint8_t *send_payload, size_t send_len,
                                          uint8_t *recv_cmd, uint8_t *recv_channel,
                                          uint8_t *recv_payload, size_t *recv_len,
                                          size_t max_recv_len, int timeout_ms)
{
    enum skyrc_usb_error err = skyrc_usb_send_frame(device, send_cmd, channel, send_payload, send_len);
    if (err != SKYRC_USB_OK) return err;

    /* Loop reading packets until we get a response matching the requested command or an ACK */
    int remaining_ms = timeout_ms;
    const int step_ms = 50;

    while (remaining_ms > 0) {
        uint8_t rcmd = 0;
        uint8_t rch = 0;
        size_t rlen = 0;

        err = skyrc_usb_receive_frame(device, &rcmd, &rch, recv_payload, &rlen, max_recv_len, step_ms);
        if (err == SKYRC_USB_OK) {
            if (rcmd == send_cmd || rcmd == 0xF0 /* ACK */) {
                if (recv_cmd) *recv_cmd = rcmd;
                if (recv_channel) *recv_channel = rch;
                if (recv_len) *recv_len = rlen;
                return SKYRC_USB_OK;
            }
        } else if (err != SKYRC_USB_TIMEOUT) {
            return err;
        }

        remaining_ms -= step_ms;
    }

    return SKYRC_USB_TIMEOUT;
}

/* ========================================================================== */
/* Device Info Implementation (CMD 87 / 0x57)                                 */
/* ========================================================================== */

skyrc_usb_device_info *skyrc_usb_device_info_create(void)
{
    return calloc(1, sizeof(skyrc_usb_device_info));
}

void skyrc_usb_device_info_free(skyrc_usb_device_info *info)
{
    free(info);
}

const char *skyrc_usb_device_info_get_core_type(const skyrc_usb_device_info *info)
{
    return info ? info->core_type : "";
}

const char *skyrc_usb_device_info_get_model_name(const skyrc_usb_device_info *info)
{
    return info ? info->model_name : "Unknown";
}

const char *skyrc_usb_device_info_get_machine_id(const skyrc_usb_device_info *info)
{
    return info ? info->machine_id : "";
}

float skyrc_usb_device_info_get_software_version(const skyrc_usb_device_info *info)
{
    return info ? info->software_version : 0.0f;
}

float skyrc_usb_device_info_get_firmware_version(const skyrc_usb_device_info *info)
{
    return skyrc_usb_device_info_get_software_version(info);
}

uint8_t skyrc_usb_device_info_get_hardware_version(const skyrc_usb_device_info *info)
{
    return info ? info->hardware_version : 0;
}

uint16_t skyrc_usb_device_info_get_customer_id(const skyrc_usb_device_info *info)
{
    return info ? info->customer_id : 0;
}

uint8_t skyrc_usb_device_info_get_language_id(const skyrc_usb_device_info *info)
{
    return info ? info->language_id : 0;
}

uint8_t skyrc_usb_device_info_get_upgrade_type(const skyrc_usb_device_info *info)
{
    return info ? info->upgrade_type : 0;
}

bool skyrc_usb_device_info_is_encrypted(const skyrc_usb_device_info *info)
{
    return info ? info->is_encrypt : false;
}

enum skyrc_usb_error skyrc_usb_get_device_info(skyrc_usb_device *device, skyrc_usb_device_info *info)
{
    if (!device || !info) return SKYRC_USB_INVALID_ARGUMENT;

    uint8_t rcmd = 0;
    uint8_t rch = 0;
    uint8_t resp[SKYRC_USB_REPORT_SIZE];
    size_t rlen = 0;

    enum skyrc_usb_error err = skyrc_usb_transceive(device, 87, 0, NULL, 0,
                                                   &rcmd, &rch, resp, &rlen, sizeof(resp),
                                                   SKYRC_USB_DEFAULT_TIMEOUT_MS);
    if (err != SKYRC_USB_OK) return err;

    /*
     * Response payload starting after channel byte:
     * In framed response:
     *   resp[0..5]: Core Type (ASCII string, 6 chars)
     *   resp[6]: Upgrade type
     *   resp[7]: Encrypt flag (1=true)
     *   resp[8..9]: Customer ID (uint16 BE)
     *   resp[10]: Language ID
     *   resp[11]: Software version major
     *   resp[12]: Software version minor (divide by 100)
     *   resp[13]: Hardware version
     *   resp[14]: Reserved
     *   resp[15]: Two's complement Checksum
     */
    if (rlen < 16) return SKYRC_USB_INVALID_PACKET;

    const uint8_t *p_core = resp;
    if (resp[0] == 0x00 && resp[1] >= '0' && resp[1] <= '9') {
        if (rlen < 17) return SKYRC_USB_INVALID_PACKET;
        p_core = &resp[1];
    }

    memcpy(info->core_type, p_core, 6);
    info->core_type[6] = '\0';

    info->upgrade_type = p_core[6];
    info->is_encrypt = (p_core[7] == 1);
    info->customer_id = (uint16_t)(((uint16_t)p_core[8] << 8) | p_core[9]);
    info->language_id = p_core[10];
    info->software_version = (float)p_core[11] + (float)p_core[12] / 100.0f;
    info->hardware_version = p_core[13];

    /* Build 32-character Machine ID string (15 hex bytes + two's complement checksum byte) */
    uint8_t sum = 0;
    char hex[33];
    for (size_t i = 0; i < 15; ++i) {
        snprintf(&hex[i * 2], 3, "%02x", p_core[i]);
        sum = (uint8_t)(sum + p_core[i]);
    }
    uint8_t inv_chk = (uint8_t)(((uint8_t)~sum) + 1u);
    snprintf(&hex[30], 3, "%02x", inv_chk);
    memcpy(info->machine_id, hex, 33);

    const char *model = skyrc_usb_lookup_model_name(info->core_type);
    snprintf(info->model_name, sizeof(info->model_name), "%s", model);

    return SKYRC_USB_OK;
}

/* ========================================================================== */
/* Live Telemetry Implementation (CMD 85 / 0x55)                              */
/* ========================================================================== */

skyrc_usb_real_data *skyrc_usb_real_data_create(void)
{
    return calloc(1, sizeof(skyrc_usb_real_data));
}

void skyrc_usb_real_data_free(skyrc_usb_real_data *data)
{
    free(data);
}

uint8_t skyrc_usb_real_data_get_work_mode(const skyrc_usb_real_data *data)
{
    return data ? data->work_mode : 0;
}

bool skyrc_usb_real_data_is_working(const skyrc_usb_real_data *data)
{
    return data ? data->is_working : false;
}

bool skyrc_usb_real_data_is_finished(const skyrc_usb_real_data *data)
{
    return data ? data->is_finished : false;
}

uint16_t skyrc_usb_real_data_get_time(const skyrc_usb_real_data *data)
{
    return data ? data->time_seconds : 0;
}

float skyrc_usb_real_data_get_voltage(const skyrc_usb_real_data *data)
{
    return data ? data->voltage : 0.0f;
}

float skyrc_usb_real_data_get_current(const skyrc_usb_real_data *data)
{
    return data ? data->current : 0.0f;
}

uint16_t skyrc_usb_real_data_get_capacity(const skyrc_usb_real_data *data)
{
    return data ? data->capacity_mah : 0;
}

uint8_t skyrc_usb_real_data_get_ext_temp(const skyrc_usb_real_data *data)
{
    return data ? data->ext_temp : 0;
}

uint8_t skyrc_usb_real_data_get_int_temp(const skyrc_usb_real_data *data)
{
    return data ? data->int_temp : 0;
}

uint16_t skyrc_usb_real_data_get_internal_resistance(const skyrc_usb_real_data *data)
{
    return data ? data->internal_resistance_mohm : 0;
}

size_t skyrc_usb_real_data_get_cell_count(const skyrc_usb_real_data *data)
{
    return data ? data->cell_count : 0;
}

bool skyrc_usb_real_data_get_cell(const skyrc_usb_real_data *data, size_t cell_index, uint16_t *value_mv)
{
    if (!data || cell_index >= SKYRC_USB_MAX_CELLS || !value_mv) return false;
    *value_mv = data->cells_mv[cell_index];
    return true;
}

uint16_t skyrc_usb_real_data_get_error_code(const skyrc_usb_real_data *data)
{
    return data ? data->error_code : 0;
}

const char *skyrc_usb_real_data_get_error_string(const skyrc_usb_real_data *data)
{
    return data ? data->error_string : NULL;
}

static const char *skyrc_b6_error_string(uint16_t code)
{
    switch (code) {
        case 256: return "Internal temperature too high";
        case 512: return "External temperature too high";
        case 768: return "DC input too low";
        case 1024: return "DC input too high";
        case 1280: return "Over time limit";
        case 1536: return "Over charge capacity limit";
        case 1792: return "Reverse polarity";
        case 2048: return "Control failure";
        case 2304: return "Breakdown";
        case 4096: return "Input failure";
        case 11: return "Connection break";
        case 12: return "Cell error voltage invalid";
        case 13: return "Balance connect error";
        case 14: return "No battery";
        case 15: return "Cell number incorrect";
        case 16: return "Connect error check main port";
        case 17: return "Battery is full";
        case 18: return "No charge needed";
        case 19: return "Cell error high voltage";
        case 20:
        case 21:
        case 22: return "Connection break";
        default: return "Fault / Error";
    }
}

enum skyrc_usb_error skyrc_usb_get_real_data(skyrc_usb_device *device,
                                             uint8_t channel,
                                             skyrc_usb_real_data *data)
{
    if (!device || !data) return SKYRC_USB_INVALID_ARGUMENT;

    uint8_t rcmd = 0;
    uint8_t rch = 0;
    uint8_t resp[SKYRC_USB_REPORT_SIZE];
    size_t rlen = 0;

    enum skyrc_usb_error err = skyrc_usb_transceive(device, 85, channel, NULL, 0,
                                                   &rcmd, &rch, resp, &rlen, sizeof(resp),
                                                   SKYRC_USB_DEFAULT_TIMEOUT_MS);
    if (err != SKYRC_USB_OK) return err;

    if (rlen < 9) return SKYRC_USB_INVALID_PACKET;

    memset(data, 0, sizeof(*data));

    uint8_t mode = resp[0];
    data->work_mode = mode;

    if (rlen >= 28) {
        /* Single-channel (B6 Mini / CM1 / CM2) layout:
         * resp[0]: workState (0=Standby, 1=Charging, 2=Idle/Ready, 3=Complete, 4=Fault)
         */
        if (mode == 4) {
            uint16_t code = (uint16_t)(((uint16_t)resp[1] << 8) | resp[2]);
            data->error_code = code;
            data->error_string = skyrc_b6_error_string(code);
            data->is_working = false;
            data->is_finished = true;
        } else if (mode == 1) {
            /* Active charging */
            data->is_working = true;
            data->is_finished = false;
            data->capacity_mah = (uint16_t)(((uint16_t)resp[1] << 8) | resp[2]);
            data->time_seconds = (uint16_t)(((uint16_t)resp[3] << 8) | resp[4]);
            uint16_t raw_v = (uint16_t)(((uint16_t)resp[5] << 8) | resp[6]);
            data->voltage = (float)raw_v / 1000.0f;
            uint16_t raw_i = (uint16_t)(((uint16_t)resp[7] << 8) | resp[8]);
            data->current = (float)raw_i / 1000.0f;
            data->ext_temp = resp[9];
            data->int_temp = resp[10];
            data->internal_resistance_mohm = (uint16_t)(((uint16_t)resp[11] << 8) | resp[12]);

            size_t cell_offset = 13;
            size_t cells_found = 0;
            while (cell_offset + 1 < rlen && cells_found < SKYRC_USB_MAX_CELLS) {
                uint16_t cv = (uint16_t)(((uint16_t)resp[cell_offset] << 8) | resp[cell_offset + 1]);
                if (cv >= 2500 && cv < 10000) {
                    data->cells_mv[cells_found++] = cv;
                }
                cell_offset += 2;
            }
            data->cell_count = cells_found;
        } else {
            /* Standby / Idle / Complete (mode 0, 2, 3) */
            data->is_working = false;
            data->is_finished = (mode == 3);
            data->capacity_mah = 0;
            data->time_seconds = 0;
            data->current = 0.0f;

            if (rlen >= 16) {
                uint16_t raw_v = (uint16_t)(((uint16_t)resp[14] << 8) | resp[15]);
                data->voltage = (float)raw_v / 1000.0f;
            }

            size_t cell_offset = 16;
            size_t cells_found = 0;
            while (cell_offset + 1 < rlen && cells_found < SKYRC_USB_MAX_CELLS) {
                uint16_t cv = (uint16_t)(((uint16_t)resp[cell_offset] << 8) | resp[cell_offset + 1]);
                if (cv >= 2500 && cv < 10000) {
                    data->cells_mv[cells_found++] = cv;
                }
                cell_offset += 2;
            }
            data->cell_count = cells_found;
        }
    } else {
        /* Multi-channel (CM3) layout */
        data->is_working = (mode == 1 || mode == 2);
        data->is_finished = (mode == 3 || mode == 4);

        uint16_t raw_time = (uint16_t)(((uint16_t)resp[1] << 8) | resp[2]);
        data->time_seconds = (raw_time > 0) ? (uint16_t)(raw_time - 1) : 0;

        uint16_t raw_v = (uint16_t)(((uint16_t)resp[3] << 8) | resp[4]);
        data->voltage = (float)raw_v / 1000.0f;

        uint16_t raw_i = (uint16_t)(((uint16_t)resp[5] << 8) | resp[6]);
        data->current = (float)raw_i / 1000.0f;

        data->capacity_mah = (uint16_t)(((uint16_t)resp[7] << 8) | resp[8]);

        if (rlen > 9) data->ext_temp = resp[9];
        if (rlen > 10) data->int_temp = resp[10];
        if (rlen > 12) {
            data->internal_resistance_mohm = (uint16_t)(((uint16_t)resp[11] << 8) | resp[12]);
        }

        size_t cell_offset = 13;
        size_t cells_found = 0;
        while (cell_offset + 1 < rlen && cells_found < SKYRC_USB_MAX_CELLS) {
            uint16_t cv = (uint16_t)(((uint16_t)resp[cell_offset] << 8) | resp[cell_offset + 1]);
            if (cv > 0 && cv < 10000) {
                data->cells_mv[cells_found++] = cv;
            }
            cell_offset += 2;
        }
        data->cell_count = cells_found;

        if (mode >= 128) {
            uint8_t err_idx = (uint8_t)(mode - 128 + 1);
            data->error_code = err_idx;
            if (err_idx > 0 && err_idx <= 32) {
                data->error_string = SKYRC_ERROR_STRINGS[err_idx - 1];
            } else {
                data->error_string = "Generic Error";
            }
        }
    }

    return SKYRC_USB_OK;
}

/* ========================================================================== */
/* Current State Implementation (CMD 95 / 0x5F)                               */
/* ========================================================================== */

skyrc_usb_current_state *skyrc_usb_current_state_create(void)
{
    return calloc(1, sizeof(skyrc_usb_current_state));
}

void skyrc_usb_current_state_free(skyrc_usb_current_state *state)
{
    free(state);
}

uint8_t skyrc_usb_current_state_get_work_mode(const skyrc_usb_current_state *state)
{
    return state ? state->work_mode : 0;
}

uint16_t skyrc_usb_current_state_get_max_charge_current(const skyrc_usb_current_state *state)
{
    return state ? state->max_charge_current_ma : 0;
}

uint16_t skyrc_usb_current_state_get_max_discharge_current(const skyrc_usb_current_state *state)
{
    return state ? state->max_discharge_current_ma : 0;
}

bool skyrc_usb_current_state_get_cell(const skyrc_usb_current_state *state, size_t cell_index, uint16_t *value_mv)
{
    if (!state || cell_index >= SKYRC_USB_MAX_CELLS || !value_mv) return false;
    *value_mv = state->cells_mv[cell_index];
    return true;
}

enum skyrc_usb_error skyrc_usb_get_current_state(skyrc_usb_device *device,
                                                 uint8_t channel,
                                                 skyrc_usb_current_state *state)
{
    if (!device || !state) return SKYRC_USB_INVALID_ARGUMENT;

    uint8_t rcmd = 0;
    uint8_t rch = 0;
    uint8_t resp[SKYRC_USB_REPORT_SIZE];
    size_t rlen = 0;

    enum skyrc_usb_error err = skyrc_usb_transceive(device, 95, channel, NULL, 0,
                                                   &rcmd, &rch, resp, &rlen, sizeof(resp),
                                                   SKYRC_USB_DEFAULT_TIMEOUT_MS);
    if (err != SKYRC_USB_OK) return err;
    if (rlen < 6) return SKYRC_USB_INVALID_PACKET;

    memset(state, 0, sizeof(*state));
    state->work_mode = resp[0];

    if (rlen > 27) {
        /* Multi-channel (CM3) layout */
        size_t idx = 1;
        for (size_t c = 0; c < 6 && idx + 1 < rlen; ++c) {
            state->cells_mv[c] = (uint16_t)(((uint16_t)resp[idx] << 8) | resp[idx + 1]);
            idx += 2;
        }
        state->max_charge_current_ma = (uint16_t)((uint16_t)resp[26] * 100u);
        state->max_discharge_current_ma = (uint16_t)((uint16_t)resp[27] * 100u);
    } else {
        /* Single-channel (B6 Mini / CM1 / CM2) layout:
         * resp[0]: work_mode
         * resp[4]: max_charge_current (in 0.1A -> * 100 to get mA)
         * resp[5]: max_discharge_current (in 0.1A -> * 100 to get mA)
         */
        state->max_charge_current_ma = (uint16_t)((uint16_t)resp[4] * 100u);
        state->max_discharge_current_ma = (uint16_t)((uint16_t)resp[5] * 100u);
    }

    return SKYRC_USB_OK;
}

/* ========================================================================== */
/* System Info Implementation (CMD 90 / 0x5A & CMD 17 / 0x11)                 */
/* ========================================================================== */

skyrc_usb_system_info *skyrc_usb_system_info_create(void)
{
    return calloc(1, sizeof(skyrc_usb_system_info));
}

void skyrc_usb_system_info_free(skyrc_usb_system_info *info)
{
    free(info);
}

bool skyrc_usb_system_info_get_key_beep(const skyrc_usb_system_info *info)
{
    return info ? info->key_beep : false;
}

bool skyrc_usb_system_info_get_system_beep(const skyrc_usb_system_info *info)
{
    return info ? info->system_beep : false;
}

bool skyrc_usb_system_info_get_capacity_enabled(const skyrc_usb_system_info *info)
{
    return info ? info->capacity_enabled : false;
}

uint16_t skyrc_usb_system_info_get_capacity_mah(const skyrc_usb_system_info *info)
{
    return info ? info->capacity_mah : 0;
}

bool skyrc_usb_system_info_get_time_protection_enabled(const skyrc_usb_system_info *info)
{
    return info ? info->time_protection_enabled : false;
}

uint16_t skyrc_usb_system_info_get_time_protection_minutes(const skyrc_usb_system_info *info)
{
    return info ? info->time_protection_minutes : 0;
}

bool skyrc_usb_system_info_get_temp_protection_enabled(const skyrc_usb_system_info *info)
{
    return info ? info->temp_protection_enabled : false;
}

uint8_t skyrc_usb_system_info_get_temp_protection_threshold(const skyrc_usb_system_info *info)
{
    return info ? info->temp_threshold_c : 0;
}

uint8_t skyrc_usb_system_info_get_rest_time_minutes(const skyrc_usb_system_info *info)
{
    return info ? info->rest_time_minutes : 0;
}

uint8_t skyrc_usb_system_info_get_dc_input_low(const skyrc_usb_system_info *info)
{
    return info ? info->dc_input_low : 0;
}

uint8_t skyrc_usb_system_info_get_nimh_peak(const skyrc_usb_system_info *info)
{
    return info ? info->nimh_peak_mv : 0;
}

uint8_t skyrc_usb_system_info_get_nicd_peak(const skyrc_usb_system_info *info)
{
    return info ? info->nicd_peak_mv : 0;
}

uint8_t skyrc_usb_system_info_get_ac_power(const skyrc_usb_system_info *info)
{
    return info ? info->ac_power_w : 0;
}

enum skyrc_usb_error skyrc_usb_get_system_info(skyrc_usb_device *device,
                                               uint8_t channel,
                                               skyrc_usb_system_info *info)
{
    if (!device || !info) return SKYRC_USB_INVALID_ARGUMENT;

    uint8_t rcmd = 0;
    uint8_t rch = 0;
    uint8_t resp[SKYRC_USB_REPORT_SIZE];
    size_t rlen = 0;

    enum skyrc_usb_error err = skyrc_usb_transceive(device, 90, channel, NULL, 0,
                                                   &rcmd, &rch, resp, &rlen, sizeof(resp),
                                                   SKYRC_USB_DEFAULT_TIMEOUT_MS);
    if (err != SKYRC_USB_OK) return err;
    if (rlen < 14) return SKYRC_USB_INVALID_PACKET;

    memset(info, 0, sizeof(*info));

    if (rlen >= 28) {
        /* Single-channel (B6 Mini / CM1 / CM2) layout:
         * resp[0]: rest_time (minutes)
         * resp[1]: time_protection_enabled (bool)
         * resp[2..3]: time_protection_minutes (uint16 BE)
         * resp[4]: capacity_enabled (bool)
         * resp[5..6]: capacity_mah (uint16 BE)
         * resp[7]: key_beep (bool)
         * resp[8]: system_beep (bool)
         * resp[9..10]: dc_input_low (uint16 BE in mV)
         * resp[13]: temp_threshold_c
         */
        info->rest_time_minutes = resp[0];
        info->time_protection_enabled = (resp[1] != 0);
        info->time_protection_minutes = (uint16_t)(((uint16_t)resp[2] << 8) | resp[3]);
        info->capacity_enabled = (resp[4] != 0);
        info->capacity_mah = (uint16_t)(((uint16_t)resp[5] << 8) | resp[6]);
        info->key_beep = (resp[7] != 0);
        info->system_beep = (resp[8] != 0);
        uint16_t raw_dc = (uint16_t)(((uint16_t)resp[9] << 8) | resp[10]);
        info->dc_input_low = (uint8_t)(raw_dc / 1000u);
        if (rlen > 13) {
            info->temp_threshold_c = resp[13];
            info->temp_protection_enabled = true;
        }
    } else {
        /* Multi-channel (CM3) layout */
        info->key_beep = (resp[0] != 0);
        info->system_beep = (resp[1] != 0);
        info->capacity_enabled = (resp[2] != 0);
        info->time_protection_enabled = (resp[3] != 0);
        /* resp[4] is temperature unit (0=C, 1=F) */
        info->capacity_mah = (uint16_t)(((uint16_t)resp[5] << 8) | resp[6]);
        info->time_protection_minutes = (uint16_t)(((uint16_t)resp[7] << 8) | resp[8]);
        info->temp_threshold_c = resp[9];
        info->rest_time_minutes = resp[10];
        info->dc_input_low = resp[11];
        info->nimh_peak_mv = resp[12];
        info->nicd_peak_mv = resp[13];
        if (rlen > 14) info->ac_power_w = resp[14];
        if (rlen > 15) info->temp_protection_enabled = (resp[15] != 0);
    }

    return SKYRC_USB_OK;
}

static enum skyrc_usb_error skyrc_usb_send_sys_setting(skyrc_usb_device *device,
                                                       uint8_t subcmd, uint8_t channel,
                                                       const uint8_t *args, size_t args_len)
{
    if (!device || !device->handle) return SKYRC_USB_INVALID_ARGUMENT;
    if (args_len > 50) return SKYRC_USB_INVALID_ARGUMENT;

    uint8_t buffer[SKYRC_USB_REPORT_SIZE + 1];
    memset(buffer, 0, sizeof(buffer));

    buffer[0] = 0x00;
    buffer[1] = 0x0F;

    /* Frame length: in Charger Master, num2 is (6 + args_len), array[2] is (num2 - 2) = (4 + args_len) */
    uint8_t frame_len = (uint8_t)(args_len + 4);
    buffer[2] = frame_len;
    buffer[3] = 17;      /* CMD 17 */
    buffer[4] = subcmd;  /* Subcommand: 0=rest_time, 1=time, 2=capacity, 3=buzzer, 5=temp */
    buffer[5] = channel; /* Channel index */

    if (args && args_len > 0) {
        memcpy(&buffer[6], args, args_len);
    }

    uint8_t chk = 0;
    size_t chk_end = 6 + args_len;
    for (size_t i = 3; i < chk_end; ++i) {
        chk = (uint8_t)(chk + buffer[i]);
    }

    buffer[chk_end] = chk;
    buffer[chk_end + 1] = 0xFF;
    buffer[chk_end + 2] = 0xFF;

    int written = hid_write(device->handle, buffer, sizeof(buffer));
    if (written < 0) return SKYRC_USB_WRITE_FAILED;

    /* Consume optional ACK response */
    uint8_t rcmd = 0, rch = 0, resp[SKYRC_USB_REPORT_SIZE];
    size_t rlen = 0;
    skyrc_usb_receive_frame(device, &rcmd, &rch, resp, &rlen, sizeof(resp), 100);

    return SKYRC_USB_OK;
}

enum skyrc_usb_error skyrc_usb_set_speaker(skyrc_usb_device *device, uint8_t channel,
                                           bool key_beep, bool buzzer)
{
    uint8_t args[2];
    args[0] = key_beep ? 1 : 0;
    args[1] = buzzer ? 1 : 0;
    return skyrc_usb_send_sys_setting(device, 3, channel, args, sizeof(args));
}

enum skyrc_usb_error skyrc_usb_set_capacity_protection(skyrc_usb_device *device, uint8_t channel,
                                                      bool enabled, uint16_t limit_mah)
{
    uint8_t args[3];
    args[0] = enabled ? 1 : 0;
    args[1] = (uint8_t)(limit_mah >> 8);
    args[2] = (uint8_t)(limit_mah & 0xFF);
    return skyrc_usb_send_sys_setting(device, 2, channel, args, sizeof(args));
}

enum skyrc_usb_error skyrc_usb_set_time_protection(skyrc_usb_device *device, uint8_t channel,
                                                  bool enabled, uint16_t minutes)
{
    uint8_t args[3];
    args[0] = enabled ? 1 : 0;
    args[1] = (uint8_t)(minutes >> 8);
    args[2] = (uint8_t)(minutes & 0xFF);
    return skyrc_usb_send_sys_setting(device, 1, channel, args, sizeof(args));
}

enum skyrc_usb_error skyrc_usb_set_rest_time(skyrc_usb_device *device, uint8_t channel, uint8_t minutes)
{
    uint8_t args[1];
    args[0] = minutes;
    return skyrc_usb_send_sys_setting(device, 0, channel, args, sizeof(args));
}

enum skyrc_usb_error skyrc_usb_set_temp_protection(skyrc_usb_device *device, uint8_t channel,
                                                  bool enabled, uint8_t threshold_c)
{
    (void)enabled;
    uint8_t args[1];
    args[0] = threshold_c;
    return skyrc_usb_send_sys_setting(device, 5, channel, args, sizeof(args));
}

/* ========================================================================== */
/* Start & Stop Program Implementation (CMD 5 & CMD 254)                      */
/* ========================================================================== */

enum skyrc_usb_error skyrc_usb_start(skyrc_usb_device *device,
                                     uint8_t channel,
                                     const skyrc_usb_charge_params *params)
{
    if (!device || !params) return SKYRC_USB_INVALID_ARGUMENT;

    /*
     * CMD 5 Payload (19 bytes matching Charger Master and live USB captures):
     *   p[0]: battery_type (0=LiPo, 1=LiIo, 2=LiFe, 3=LiHV, 4=NiMH, 5=NiCd, 6=Pb)
     *   p[1]: cell_count
     *   p[2]: mode (Charge, Discharge, Storage, etc.)
     *   p[3..4]: charge current (in units of 10mA, e.g. 1000 mA -> 100, BE)
     *   p[5..6]: discharge current (in units of 10mA, e.g. 1000 mA -> 100, BE)
     *   p[7..8]: cell voltage (mV, BE, e.g. 3200)
     *   p[9..10]: end voltage (mV, BE, e.g. 4200)
     *   p[11]: cycle count / repeat peak
     *   p[12]: cycle type / count
     *   p[13..14]: trickle current (mA, BE)
     *   p[15..18]: zero padding (4 bytes)
     */
    uint8_t p[19];
    memset(p, 0, sizeof(p));

    p[0] = params->battery_type;
    p[1] = params->cell_count;
    p[2] = params->mode;

    uint16_t c_cur = (uint16_t)(params->charge_current_ma / 10u);
    p[3] = (uint8_t)(c_cur >> 8);
    p[4] = (uint8_t)(c_cur & 0xFF);

    uint16_t d_cur = (uint16_t)(params->discharge_current_ma / 10u);
    p[5] = (uint8_t)(d_cur >> 8);
    p[6] = (uint8_t)(d_cur & 0xFF);

    p[7] = (uint8_t)(params->cutoff_voltage_mv >> 8);
    p[8] = (uint8_t)(params->cutoff_voltage_mv & 0xFF);
    p[9] = (uint8_t)(params->end_voltage_mv >> 8);
    p[10] = (uint8_t)(params->end_voltage_mv & 0xFF);
    p[11] = params->cycle_count;
    p[12] = params->cycle_delay_min;
    p[13] = (uint8_t)(params->trickle_current_ma >> 8);
    p[14] = (uint8_t)(params->trickle_current_ma & 0xFF);

    return skyrc_usb_send_frame(device, 5, channel, p, sizeof(p));
}

enum skyrc_usb_error skyrc_usb_stop(skyrc_usb_device *device, uint8_t channel)
{
    /* CMD 254 (0xFE) stops current charging/discharging */
    return skyrc_usb_send_frame(device, 254, channel, NULL, 0);
}

/* ========================================================================== */
/* Operations Table                                                           */
/* ========================================================================== */

static const char *const BATTERY_TYPES[] = {
    "LiPo", "LiIo", "LiFe", "LiHV", "NiMH", "NiCd", "Pb", "Smart/DJI", NULL
};

static const char *const MODES_LIPO[] = {
    "Charge", "Discharge", "Storage", "Fast Charge", "Balance Charge", NULL
};

static const char *const MODES_NIMH[] = {
    "Charge", "Auto Charge", "Discharge", "Re-Peak", "Cycle", NULL
};

static const char *const MODES_PB[] = {
    "Charge", "Discharge", "AGM Charge", "Cold Charge", NULL
};

static const char *const MODES_SMART[] = {
    "Storage", "Charge", NULL
};

static const char *const *const ALL_MODES[] = {
    MODES_LIPO,  /* LiPo */
    MODES_LIPO,  /* LiIo */
    MODES_LIPO,  /* LiFe */
    MODES_LIPO,  /* LiHV */
    MODES_NIMH,  /* NiMH */
    MODES_NIMH,  /* NiCd */
    MODES_PB,    /* Pb */
    MODES_SMART, /* Smart */
    NULL
};

static const skyrc_usb_operations DEFAULT_OPERATIONS = {
    .battery_types = BATTERY_TYPES,
    .battery_type_count = 8,
    .modes = ALL_MODES
};

const skyrc_usb_operations *skyrc_usb_get_operations(const char *model_or_core)
{
    (void)model_or_core;
    return &DEFAULT_OPERATIONS;
}

size_t skyrc_usb_operations_get_battery_type_count(const skyrc_usb_operations *ops)
{
    return ops ? ops->battery_type_count : 0;
}

const char *skyrc_usb_operations_get_battery_type(const skyrc_usb_operations *ops, size_t index)
{
    if (!ops || index >= ops->battery_type_count) return NULL;
    return ops->battery_types[index];
}

size_t skyrc_usb_operations_get_mode_count(const skyrc_usb_operations *ops, size_t battery_index)
{
    if (!ops || battery_index >= ops->battery_type_count) return 0;
    const char *const *m = ops->modes[battery_index];
    if (!m) return 0;
    size_t count = 0;
    while (m[count] != NULL) count++;
    return count;
}

const char *skyrc_usb_operations_get_mode(const skyrc_usb_operations *ops,
                                          size_t battery_index, size_t mode_index)
{
    if (!ops || battery_index >= ops->battery_type_count) return NULL;
    const char *const *m = ops->modes[battery_index];
    if (!m) return NULL;
    size_t count = 0;
    while (m[count] != NULL) {
        if (count == mode_index) return m[count];
        count++;
    }
    return NULL;
}
