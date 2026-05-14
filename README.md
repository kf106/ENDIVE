# ESP32-S3 Super Mini — USB MSC + CDC “EnvDrive”

Firmware for **ESP32-S3 Super Mini** class boards (4 MB flash, USB-C on the native USB pins). At boot the chip keeps **USB Serial/JTAG** on the USB port so you can flash and monitor with `idf.py`. After a reset, press **BOOT (GPIO0)** to start a **USB composite device**: a removable **FAT** volume (wear-leveled internal flash) plus a **CDC serial** port. The host sees the disk as **EnvDrive**.

Session length, warning blink timing, status LED behavior, and host eject handling are documented in the source header in `main/main.c`.

## Board

These boards (or equivalents) are commonly sold as ESP32-S3 “Super Mini” modules:

[ESP32-S3 Super Mini on AliExpress](https://www.aliexpress.com/item/1005007523988592.html?spm=a2g0o.order_list.order_list_main.11.1d891802CVyvzq)

Use a **data-capable** USB-C cable. The firmware targets **4 MB flash** and **quad PSRAM** as set in `sdkconfig.defaults`.

## Requirements

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/index.html) **v5.x** (see `main/idf_component.yml` for the declared range)
- Python and CMake as required by ESP-IDF
- A serial port when flashing/monitoring (USB to the board’s native port is typical)

## Clone and build

```bash
cd esp32-s3-mini
idf.py set-target esp32s3
idf.py build
```

The first build downloads **managed components** (TinyUSB, LED strip, etc.) from the Espressif component registry.

## Flash and monitor

Connect USB, put the board in download mode if needed (**BOOT** held, tap **RESET**, release **BOOT**), then:

```bash
idf.py -p PORT flash monitor
```

Replace `PORT` with your serial device (for example `/dev/ttyACM0` on Linux).

To exit the monitor: **Ctrl+]**

## Using the USB disk and serial

1. Let the board boot; the port stays in **USB Serial/JTAG** mode for flashing.
2. Press **BOOT** to enumerate **MSC + CDC**. Mount **EnvDrive** on the host.
3. Optional timing file: **`config`** on the volume root (`session_seconds`, `warning_seconds`). If the file is missing, the firmware creates a default on first use. After editing `config`, unplug and replug USB before the next BOOT session so the host rescans reliably.
4. Ejecting the volume from the OS ends the session after a short debounce so you can start another session with BOOT without waiting for the full timer.

## Repository layout

| Path | Role |
|------|------|
| `main/main.c` | Application: TinyUSB MSC/CDC, FAT, BOOT, LEDs, timers |
| `main/idf_component.yml` | Component manager dependencies |
| `partitions.csv` | Factory app + 2 MB FAT `storage` partition |
| `sdkconfig.defaults` | Flash size, PSRAM, TinyUSB, FAT, mount path `/usb` |

Build outputs and local `sdkconfig` are ignored by `.gitignore`; share defaults via `sdkconfig.defaults`.

## License

MIT — see `LICENSE`. Copyright (c) 2026 Keir Finlow-Bates.
