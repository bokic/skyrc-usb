# libskyrc-usb

`libskyrc-usb` is a C shared library and command-line interface (`skyrc-usb`) for communicating with SkyRC battery chargers over USB HID on Linux. It is reverse-engineered from the official SkyRC Charger Master software suite (Charger Master 1, 2, 3, Q200, and T1000).

## Supported Charger Families

The library supports single-channel, dual-channel, and quad-channel SkyRC chargers, including:

- **T1000 Series:** T1000 Maestro
- **Neo Series:** D200neo, Q200neo, D100neo, S100neo, B6neo, B6ACneo, B6neo+, Q100neo, B6neo 2
- **D-Series:** D100, D200, D400, D250, D260, D100 V2
- **Q-Series:** Q200, Q200 V2
- **Classic B6 Series:** B6 Mini, B6AC V2, B6AC+, B6 (6x80+)
- **Industrial / Heavy Duty:** PC1500, PC1500neo, PC3000H, PC1080neo, PC7200, PSU2000, BD380

Discovery identifies models via the 6-character core hardware ID returned during the `CMD 87` handshake.

## Capabilities

- **Device Discovery & Enumeration:** Scan USB HID devices matching SkyRC VID `0x0000` / PID `0x0001` (or custom VID/PID) using `hidapi`.
- **Hardware Identification:** Query core type, model name, software version, hardware version, customer ID, encryption status, and unique 32-character Machine ID (`CMD 87` / `0x57`).
- **Real-Time Telemetry:** Poll pack voltage, charge/discharge current, wattage, capacity (mAh), elapsed time, external/battery temperature, internal temperature, internal resistance ($m\Omega$), per-cell voltages (up to 8S), and error strings (`CMD 85` / `0x55`).
- **Program Status:** Read active work mode, per-cell voltages, and maximum configured charge/discharge current limits (`CMD 95` / `0x5F`).
- **System Configuration:** Query and set key beep, buzzer beep, capacity cut-off limit, safety timer, temperature protection threshold, rest time, minimum DC input voltage, and AC power distribution (`CMD 90` / `0x5A` and `CMD 17` / `0x11`).
- **Program Control:** Start charge, discharge, storage, balance charge, fast charge, re-peak, and cycle operations (`CMD 5` / `0x05`), and stop active programs (`CMD 254` / `0xFE`).
- **Multi-Channel Support:** Channel-aware addressing for up to 4 independent charging ports (0 = Channel A/1, 1 = Channel B/2, 2 = Channel C/3, 3 = Channel D/4).
- **Framing & Checksum Engine:** Robust frame generation and parsing supporting both framed (`0x0F` header with additive 8-bit checksum and `0xFF 0xFF` sentinel) and command-first responses.

## Requirements

- Linux with USB HID access (`hidraw`).
- `hidapi` development files (`hidapi-hidraw`).
- CMake 3.15 or newer, Ninja, and a C23-compliant compiler (e.g. GCC 13+ or Clang 16+).

### USB Permissions (udev Rule)

To access SkyRC USB chargers as a standard user without root privileges, create `/etc/udev/rules.d/99-skyrc.rules`:

```udev
# SkyRC USB HID Chargers (VID 0000, PID 0001)
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="0000", ATTRS{idProduct}=="0001", MODE="0666", GROUP="plugdev"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0000", ATTRS{idProduct}=="0001", MODE="0666", GROUP="plugdev"
```

Reload udev rules after creating the file:

```sh
sudo udevadm control --reload-rules && sudo udevadm trigger
```

## Build

The build script compiles the shared library and the CLI tool using CMake and Ninja:

```sh
./build.sh
```

Binaries will be placed in `bin/`:
- `libskyrc-usb.so` (Shared library)
- `skyrc-usb` (Command-line tool)

To configure and build manually:

```sh
cmake -G Ninja -S . -B build -DLIBSKYRC_VERSION=1.0.0
cmake --build build
```

## Command-Line Tool (`skyrc-usb`)

The CLI provides subcommands for inspecting and controlling chargers:

```sh
# Scan for connected SkyRC USB chargers
skyrc-usb scan

# Print model, version, serial, and identity
skyrc-usb info

# Read current charger status and cell voltages on channel 1 (0-indexed channel 0)
skyrc-usb status --channel 0

# Read system configuration and protection limits
skyrc-usb system --channel 0

# Stream live measurements (voltage, current, power, capacity, temp, IR, cells)
skyrc-usb monitor --channel 0 --interval 1000

# Start a 2S LiPo Balance Charge at 2.5 A
skyrc-usb start --channel 0 --battery lipo --mode balance --cells 2 --charge-current 2.5

# Stop charging
skyrc-usb stop --channel 0

# Configure speaker beeps
skyrc-usb set-speaker --key off --buzzer on

# Set safety cutoffs
skyrc-usb set-capacity on --limit 5000
skyrc-usb set-time on --minutes 120
skyrc-usb set-temp on --threshold 50
skyrc-usb set-rest-time --minutes 10

# List supported chemistries and modes
skyrc-usb list-ops

# Send a raw frame in hexadecimal
skyrc-usb raw 57:00
```

## C Library API

Include `<skyrc-usb.h>` and link against `libskyrc-usb`:

```c
#include <skyrc-usb.h>
#include <stdio.h>

int main(void)
{
    skyrc_usb_device *dev = NULL;
    enum skyrc_usb_error err = skyrc_usb_open_default(&dev);
    if (err != SKYRC_USB_OK) {
        fprintf(stderr, "Could not open charger: %s\n", skyrc_usb_error_string(err));
        return 1;
    }

    skyrc_usb_device_info *info = skyrc_usb_device_info_create();
    if (skyrc_usb_get_device_info(dev, info) == SKYRC_USB_OK) {
        printf("Connected to %s (Core: %s, FW: %.2f, SN: %s)\n",
               skyrc_usb_device_info_get_model_name(info),
               skyrc_usb_device_info_get_core_type(info),
               (double)skyrc_usb_device_info_get_software_version(info),
               skyrc_usb_device_info_get_machine_id(info));
    }
    skyrc_usb_device_info_free(info);

    skyrc_usb_real_data *data = skyrc_usb_real_data_create();
    if (skyrc_usb_get_real_data(dev, 0, data) == SKYRC_USB_OK) {
        printf("Voltage: %.3f V, Current: %.3f A, Capacity: %u mAh\n",
               (double)skyrc_usb_real_data_get_voltage(data),
               (double)skyrc_usb_real_data_get_current(data),
               skyrc_usb_real_data_get_capacity(data));
    }
    skyrc_usb_real_data_free(data);

    skyrc_usb_close(dev);
    return 0;
}
```

## License

MIT License. See [LICENSE](LICENSE) for details.
