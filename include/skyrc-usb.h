#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef libskyrc_usb_EXPORTS
 #if defined(_MSC_VER)
  #define EXPORT_SKYRC_USB __declspec(dllexport)
 #else
  #define EXPORT_SKYRC_USB __attribute__((visibility("default")))
 #endif
#else
 #define EXPORT_SKYRC_USB
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Default SkyRC USB HID Vendor ID and Product ID. */
#define SKYRC_USB_DEFAULT_VID 0x0000
#define SKYRC_USB_DEFAULT_PID 0x0001

/** Default communication timeout in milliseconds. */
#define SKYRC_USB_DEFAULT_TIMEOUT_MS 500

/** Raw USB HID Report byte length. */
#define SKYRC_USB_REPORT_SIZE 64

/** Maximum number of battery cells supported in telemetry. */
#define SKYRC_USB_MAX_CELLS 8

/** Maximum number of channels (slots) supported by chargers (e.g. Q200). */
#define SKYRC_USB_MAX_CHANNELS 4

/** Status codes returned by USB library APIs. */
enum skyrc_usb_error {
    SKYRC_USB_OK = 0,
    SKYRC_USB_INVALID_ARGUMENT,
    SKYRC_USB_DEVICE_NOT_FOUND,
    SKYRC_USB_PERMISSION_DENIED,
    SKYRC_USB_OPEN_FAILED,
    SKYRC_USB_WRITE_FAILED,
    SKYRC_USB_READ_FAILED,
    SKYRC_USB_TIMEOUT,
    SKYRC_USB_CHECKSUM_ERROR,
    SKYRC_USB_INVALID_PACKET,
    SKYRC_USB_UNSUPPORTED_COMMAND,
    SKYRC_USB_OUT_OF_MEMORY,
};

/** Return a human-readable description of a skyrc_usb_error. */
EXPORT_SKYRC_USB const char *skyrc_usb_error_string(enum skyrc_usb_error error);

/* -------------------------------------------------------------------------- */
/* Device Enumeration & Discovery                                             */
/* -------------------------------------------------------------------------- */

/** Details of a detected SkyRC USB HID device during scan. */
typedef struct skyrc_usb_scan_result skyrc_usb_scan_result;

/** Called when a USB device is enumerated. Result is valid only during callback. */
typedef void (*skyrc_usb_scan_callback)(void *context, const skyrc_usb_scan_result *result);

/** Get the system path of the scanned USB device. */
EXPORT_SKYRC_USB const char *skyrc_usb_scan_result_get_path(const skyrc_usb_scan_result *result);

/** Get the Vendor ID of the scanned USB device. */
EXPORT_SKYRC_USB uint16_t skyrc_usb_scan_result_get_vendor_id(const skyrc_usb_scan_result *result);

/** Get the Product ID of the scanned USB device. */
EXPORT_SKYRC_USB uint16_t skyrc_usb_scan_result_get_product_id(const skyrc_usb_scan_result *result);

/** Get the serial number string of the scanned device, or NULL if unavailable. */
EXPORT_SKYRC_USB const char *skyrc_usb_scan_result_get_serial_number(const skyrc_usb_scan_result *result);

/** Get the manufacturer string of the scanned device, or NULL if unavailable. */
EXPORT_SKYRC_USB const char *skyrc_usb_scan_result_get_manufacturer_string(const skyrc_usb_scan_result *result);

/** Get the product string of the scanned device, or NULL if unavailable. */
EXPORT_SKYRC_USB const char *skyrc_usb_scan_result_get_product_string(const skyrc_usb_scan_result *result);

/** Enumerate USB HID devices matching vendor_id and product_id (or 0 for default 0000:0001). */
EXPORT_SKYRC_USB enum skyrc_usb_error skyrc_usb_scan(uint16_t vendor_id, uint16_t product_id,
                                                    skyrc_usb_scan_callback callback, void *context);

/* -------------------------------------------------------------------------- */
/* Connection Management                                                      */
/* -------------------------------------------------------------------------- */

/** Opaque handle representing an open SkyRC USB charger. */
typedef struct skyrc_usb_device skyrc_usb_device;

/** Open a charger by system path (e.g. from scan result). */
EXPORT_SKYRC_USB enum skyrc_usb_error skyrc_usb_open_path(const char *path, skyrc_usb_device **device);

/** Open a charger by Vendor ID, Product ID, and optional serial number (NULL for first matching). */
EXPORT_SKYRC_USB enum skyrc_usb_error skyrc_usb_open(uint16_t vendor_id, uint16_t product_id,
                                                    const char *serial, skyrc_usb_device **device);

/** Open the default SkyRC charger (VID 0x0000, PID 0x0001). */
EXPORT_SKYRC_USB enum skyrc_usb_error skyrc_usb_open_default(skyrc_usb_device **device);

/** Close and free a device handle. device may be NULL. */
EXPORT_SKYRC_USB void skyrc_usb_close(skyrc_usb_device *device);

/* -------------------------------------------------------------------------- */
/* Low-Level Frame Operations                                                 */
/* -------------------------------------------------------------------------- */

/**
 * Send a framed command packet to the charger.
 * Automatically wraps payload with: 0x0F, length, cmd, channel, payload, checksum, 0xFF, 0xFF.
 */
EXPORT_SKYRC_USB enum skyrc_usb_error skyrc_usb_send_frame(skyrc_usb_device *device,
                                                           uint8_t cmd, uint8_t channel,
                                                           const uint8_t *payload, size_t payload_len);

/**
 * Receive and decode a frame from the charger within timeout_ms.
 * Unpacks framed responses (starting with 0x0F) or raw responses (starting with cmd).
 */
EXPORT_SKYRC_USB enum skyrc_usb_error skyrc_usb_receive_frame(skyrc_usb_device *device,
                                                              uint8_t *cmd, uint8_t *channel,
                                                              uint8_t *payload, size_t *payload_len,
                                                              size_t max_payload_len, int timeout_ms);

/**
 * Send a command frame and wait for the matching response.
 */
EXPORT_SKYRC_USB enum skyrc_usb_error skyrc_usb_transceive(skyrc_usb_device *device,
                                                           uint8_t send_cmd, uint8_t channel,
                                                           const uint8_t *send_payload, size_t send_len,
                                                           uint8_t *recv_cmd, uint8_t *recv_channel,
                                                           uint8_t *recv_payload, size_t *recv_len,
                                                           size_t max_recv_len, int timeout_ms);

/* -------------------------------------------------------------------------- */
/* Device Identification (CMD 87 / 0x57)                                      */
/* -------------------------------------------------------------------------- */

/** Device identification information read from the charger hardware. */
typedef struct skyrc_usb_device_info skyrc_usb_device_info;

EXPORT_SKYRC_USB skyrc_usb_device_info *skyrc_usb_device_info_create(void);
EXPORT_SKYRC_USB void skyrc_usb_device_info_free(skyrc_usb_device_info *info);

/** Query and decode device identity (core type, version, machine serial). */
EXPORT_SKYRC_USB enum skyrc_usb_error skyrc_usb_get_device_info(skyrc_usb_device *device,
                                                                skyrc_usb_device_info *info);

EXPORT_SKYRC_USB const char *skyrc_usb_device_info_get_core_type(const skyrc_usb_device_info *info);
EXPORT_SKYRC_USB const char *skyrc_usb_device_info_get_model_name(const skyrc_usb_device_info *info);
EXPORT_SKYRC_USB const char *skyrc_usb_device_info_get_machine_id(const skyrc_usb_device_info *info);
EXPORT_SKYRC_USB float skyrc_usb_device_info_get_software_version(const skyrc_usb_device_info *info);
EXPORT_SKYRC_USB float skyrc_usb_device_info_get_firmware_version(const skyrc_usb_device_info *info);
EXPORT_SKYRC_USB uint8_t skyrc_usb_device_info_get_hardware_version(const skyrc_usb_device_info *info);
EXPORT_SKYRC_USB uint16_t skyrc_usb_device_info_get_customer_id(const skyrc_usb_device_info *info);
EXPORT_SKYRC_USB uint8_t skyrc_usb_device_info_get_language_id(const skyrc_usb_device_info *info);
EXPORT_SKYRC_USB uint8_t skyrc_usb_device_info_get_upgrade_type(const skyrc_usb_device_info *info);
EXPORT_SKYRC_USB bool skyrc_usb_device_info_is_encrypted(const skyrc_usb_device_info *info);

/** Map a 6-character core type string (e.g. "100182", "100084") to its model name. */
EXPORT_SKYRC_USB const char *skyrc_usb_lookup_model_name(const char *core_type);

/* -------------------------------------------------------------------------- */
/* Live Telemetry Measurements (CMD 85 / 0x55)                                */
/* -------------------------------------------------------------------------- */

/** Opaque container for live charger telemetry measurements. */
typedef struct skyrc_usb_real_data skyrc_usb_real_data;

EXPORT_SKYRC_USB skyrc_usb_real_data *skyrc_usb_real_data_create(void);
EXPORT_SKYRC_USB void skyrc_usb_real_data_free(skyrc_usb_real_data *data);

/** Poll live telemetry measurements for the specified channel (0-based: 0=A, 1=B, etc.). */
EXPORT_SKYRC_USB enum skyrc_usb_error skyrc_usb_get_real_data(skyrc_usb_device *device,
                                                              uint8_t channel,
                                                              skyrc_usb_real_data *data);

EXPORT_SKYRC_USB uint8_t skyrc_usb_real_data_get_work_mode(const skyrc_usb_real_data *data);
EXPORT_SKYRC_USB bool skyrc_usb_real_data_is_working(const skyrc_usb_real_data *data);
EXPORT_SKYRC_USB bool skyrc_usb_real_data_is_finished(const skyrc_usb_real_data *data);
EXPORT_SKYRC_USB uint16_t skyrc_usb_real_data_get_time(const skyrc_usb_real_data *data);
EXPORT_SKYRC_USB float skyrc_usb_real_data_get_voltage(const skyrc_usb_real_data *data);
EXPORT_SKYRC_USB float skyrc_usb_real_data_get_current(const skyrc_usb_real_data *data);
EXPORT_SKYRC_USB uint16_t skyrc_usb_real_data_get_capacity(const skyrc_usb_real_data *data);
EXPORT_SKYRC_USB uint8_t skyrc_usb_real_data_get_ext_temp(const skyrc_usb_real_data *data);
EXPORT_SKYRC_USB uint8_t skyrc_usb_real_data_get_int_temp(const skyrc_usb_real_data *data);
EXPORT_SKYRC_USB uint16_t skyrc_usb_real_data_get_internal_resistance(const skyrc_usb_real_data *data);
EXPORT_SKYRC_USB size_t skyrc_usb_real_data_get_cell_count(const skyrc_usb_real_data *data);
EXPORT_SKYRC_USB bool skyrc_usb_real_data_get_cell(const skyrc_usb_real_data *data,
                                                   size_t cell_index, uint16_t *value_mv);
EXPORT_SKYRC_USB uint16_t skyrc_usb_real_data_get_error_code(const skyrc_usb_real_data *data);
EXPORT_SKYRC_USB const char *skyrc_usb_real_data_get_error_string(const skyrc_usb_real_data *data);

/* -------------------------------------------------------------------------- */
/* Current Charger Program State (CMD 95 / 0x5F)                              */
/* -------------------------------------------------------------------------- */

/** Opaque container for current charger program state. */
typedef struct skyrc_usb_current_state skyrc_usb_current_state;

EXPORT_SKYRC_USB skyrc_usb_current_state *skyrc_usb_current_state_create(void);
EXPORT_SKYRC_USB void skyrc_usb_current_state_free(skyrc_usb_current_state *state);

/** Query current program state for the specified channel. */
EXPORT_SKYRC_USB enum skyrc_usb_error skyrc_usb_get_current_state(skyrc_usb_device *device,
                                                                  uint8_t channel,
                                                                  skyrc_usb_current_state *state);

EXPORT_SKYRC_USB uint8_t skyrc_usb_current_state_get_work_mode(const skyrc_usb_current_state *state);
EXPORT_SKYRC_USB uint16_t skyrc_usb_current_state_get_max_charge_current(const skyrc_usb_current_state *state);
EXPORT_SKYRC_USB uint16_t skyrc_usb_current_state_get_max_discharge_current(const skyrc_usb_current_state *state);
EXPORT_SKYRC_USB bool skyrc_usb_current_state_get_cell(const skyrc_usb_current_state *state,
                                                       size_t cell_index, uint16_t *value_mv);

/* -------------------------------------------------------------------------- */
/* System Settings & Configuration (CMD 90 / 0x5A & CMD 17 / 0x11)            */
/* -------------------------------------------------------------------------- */

/** Opaque container for system settings. */
typedef struct skyrc_usb_system_info skyrc_usb_system_info;

EXPORT_SKYRC_USB skyrc_usb_system_info *skyrc_usb_system_info_create(void);
EXPORT_SKYRC_USB void skyrc_usb_system_info_free(skyrc_usb_system_info *info);

/** Read system configuration from charger. */
EXPORT_SKYRC_USB enum skyrc_usb_error skyrc_usb_get_system_info(skyrc_usb_device *device,
                                                                uint8_t channel,
                                                                skyrc_usb_system_info *info);

EXPORT_SKYRC_USB bool skyrc_usb_system_info_get_key_beep(const skyrc_usb_system_info *info);
EXPORT_SKYRC_USB bool skyrc_usb_system_info_get_system_beep(const skyrc_usb_system_info *info);
EXPORT_SKYRC_USB bool skyrc_usb_system_info_get_capacity_enabled(const skyrc_usb_system_info *info);
EXPORT_SKYRC_USB uint16_t skyrc_usb_system_info_get_capacity_mah(const skyrc_usb_system_info *info);
EXPORT_SKYRC_USB bool skyrc_usb_system_info_get_time_protection_enabled(const skyrc_usb_system_info *info);
EXPORT_SKYRC_USB uint16_t skyrc_usb_system_info_get_time_protection_minutes(const skyrc_usb_system_info *info);
EXPORT_SKYRC_USB bool skyrc_usb_system_info_get_temp_protection_enabled(const skyrc_usb_system_info *info);
EXPORT_SKYRC_USB uint8_t skyrc_usb_system_info_get_temp_protection_threshold(const skyrc_usb_system_info *info);
EXPORT_SKYRC_USB uint8_t skyrc_usb_system_info_get_rest_time_minutes(const skyrc_usb_system_info *info);
EXPORT_SKYRC_USB uint8_t skyrc_usb_system_info_get_dc_input_low(const skyrc_usb_system_info *info);
EXPORT_SKYRC_USB uint8_t skyrc_usb_system_info_get_nimh_peak(const skyrc_usb_system_info *info);
EXPORT_SKYRC_USB uint8_t skyrc_usb_system_info_get_nicd_peak(const skyrc_usb_system_info *info);
EXPORT_SKYRC_USB uint8_t skyrc_usb_system_info_get_ac_power(const skyrc_usb_system_info *info);

/* Setter functions using CMD 17 */
EXPORT_SKYRC_USB enum skyrc_usb_error skyrc_usb_set_speaker(skyrc_usb_device *device,
                                                            uint8_t channel,
                                                            bool key_beep, bool buzzer);

EXPORT_SKYRC_USB enum skyrc_usb_error skyrc_usb_set_capacity_protection(skyrc_usb_device *device,
                                                                        uint8_t channel,
                                                                        bool enabled, uint16_t limit_mah);

EXPORT_SKYRC_USB enum skyrc_usb_error skyrc_usb_set_time_protection(skyrc_usb_device *device,
                                                                    uint8_t channel,
                                                                    bool enabled, uint16_t minutes);

EXPORT_SKYRC_USB enum skyrc_usb_error skyrc_usb_set_rest_time(skyrc_usb_device *device,
                                                              uint8_t channel,
                                                              uint8_t minutes);

EXPORT_SKYRC_USB enum skyrc_usb_error skyrc_usb_set_temp_protection(skyrc_usb_device *device,
                                                                    uint8_t channel,
                                                                    bool enabled, uint8_t threshold_c);

/* -------------------------------------------------------------------------- */
/* Start & Stop Program Operations                                            */
/* -------------------------------------------------------------------------- */

/** Parameters for starting a charge/discharge/cycle operation. */
typedef struct skyrc_usb_charge_params {
    uint8_t battery_type;        /**< 0=LiPo, 1=LiIo, 2=LiFe, 3=LiHV, 4=NiMH, 5=NiCd, 6=Pb */
    uint8_t cell_count;          /**< Number of cells (e.g. 1-8 for LiPo) */
    uint8_t mode;                /**< Operation mode (0=Charge, 1=Discharge, etc.) */
    uint16_t charge_current_ma;   /**< Charge current in mA */
    uint16_t discharge_current_ma;/**< Discharge current in mA */
    uint16_t cutoff_voltage_mv;  /**< Cell cutoff voltage in mV (or 0 for default) */
    uint16_t end_voltage_mv;     /**< End voltage in mV (or 0 for default) */
    uint16_t trickle_current_ma; /**< NiMH/NiCd trickle current in mA */
    uint8_t cycle_count;         /**< Cycle count (1-5) */
    uint8_t cycle_delay_min;     /**< Cycle delay in minutes */
    uint8_t peak_sensitivity_mv; /**< Peak sensitivity in mV */
} skyrc_usb_charge_params;

/** Start a charging or discharging program on the charger channel. */
EXPORT_SKYRC_USB enum skyrc_usb_error skyrc_usb_start(skyrc_usb_device *device,
                                                      uint8_t channel,
                                                      const skyrc_usb_charge_params *params);

/** Stop the currently running program on the specified channel. */
EXPORT_SKYRC_USB enum skyrc_usb_error skyrc_usb_stop(skyrc_usb_device *device, uint8_t channel);

/* -------------------------------------------------------------------------- */
/* Model Operations Metadata                                                  */
/* -------------------------------------------------------------------------- */

typedef struct skyrc_usb_operations skyrc_usb_operations;

EXPORT_SKYRC_USB const skyrc_usb_operations *skyrc_usb_get_operations(const char *model_or_core);
EXPORT_SKYRC_USB size_t skyrc_usb_operations_get_battery_type_count(const skyrc_usb_operations *ops);
EXPORT_SKYRC_USB const char *skyrc_usb_operations_get_battery_type(const skyrc_usb_operations *ops,
                                                                   size_t index);
EXPORT_SKYRC_USB size_t skyrc_usb_operations_get_mode_count(const skyrc_usb_operations *ops,
                                                            size_t battery_index);
EXPORT_SKYRC_USB const char *skyrc_usb_operations_get_mode(const skyrc_usb_operations *ops,
                                                           size_t battery_index, size_t mode_index);

#ifdef __cplusplus
}
#endif
