# ![Endive icon](img/endive-48.png) Project Endive

I finally came up with an ESP32 project that has a real purpose, and isn't just a clock or a weather station.

As a sometime-developer I have projects in repositories that connect to API services with secret API token or that need to use a private cryptographic key for something. Like every other developer out there, I stick them in a .env file, and try to remember to exlude that file from my repository using a .gitignore file.

Apart from the obvious problem that one thoughtless commit and push can reveal those secrets, there is the added problem that the .env file is always visible on my development machine. If that is compromised, so are all my tokens and keys. 

Enter the EnvDrive. 

An ESP32-S3 Super Mini board can be bought for about $3-$4, has 4 MB of flash (and typically 2 MB of PSRAM), and can be configured to act like a USB drive. That means I can use symlinks from all my projects to .env files on the board. 4MB can store a lot of ascii files.

*Couldn't you do that with a standard USB drive, though?*

Yes, but here's the clever part. The board only presents the file system after you press a button on it. And it has a timer, which disconnects the drive after a given period of time, warning you with the standard on-board LED. Red means not connected, green means connected, and flashing yellow means about to disconnect (which gives you time to press the button again).

If I used a USB drive, I'd forget to unplug it. Now I don't have to.

## Future plans

Mark Jivko liked the idea, but had a few suggestions on how to improve it:

1. Encrypt the file system on the non-volatile RAM, and decrypt it to the volatile RAM on demand. This needs some thoughts about how the decryption password can be entered. Perhaps over BLE from a phone app? Perhaps through an onboard web server?

2. Have a "call home" feature, where the device tries to obtain an internet connection, and then checks whether the device is marked as stolen. If it is, it formats itself. Again, perhas the device only works if it has internet access through your phone through an app, or something.

3. The firmware uses the boot button to trigger the mounting of the drive. That button is really awkward to press. What I'd really like is a nice case that shows the LED, and something like a TP223 touch key to replace pressing the little fiddly button. Those things are sensitive: you'll find a similar touch key on a Yubikey, for example.

## Summary
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
