# ![Endive icon](img/endive-48.png) Project Endive

I finally came up with an ESP32 project that has a real purpose, and isn't just a clock or a weather station.

As a sometime-developer I have projects in repositories that connect to API services with secret API token or that need to use a private cryptographic key for something. Like every other developer out there, I stick them in a .env file, and try to remember to exlude that file from my repository using a .gitignore file.

Apart from the obvious problem that one thoughtless commit and push can reveal those secrets, there is the added problem that the .env file is always visible on my development machine. If that is compromised, so are all my tokens and keys. 

Enter **ENDIVE**. 

An ESP32-S3 Super Mini board can be bought for about $3-$4, has 4 MB of flash (and typically 2 MB of PSRAM), and can be configured to act like a USB drive. That means I can use symlinks from all my projects to .env files on the board. 4MB can store a lot of ascii files. (Note: this works with the ESP32-S3 board, but not the ESP32-C3 board, which does not support exposing filesystems over USB)

*Couldn't you do that with a standard USB drive, though?*

Yes, but here's the clever part. The board only presents the file system after you press a button on it. And it has a timer, which disconnects the drive after a given period of time, warning you with the standard on-board LED. Red means not connected, green means connected, and flashing yellow means about to disconnect (which gives you time to press the button again).

If I used a USB drive, I'd forget to unplug it. Now I don't have to remember.

## Future plans

Mark Jivko liked the idea, but had a few suggestions on how to improve it:

1. Encrypt the file system on the non-volatile RAM, and decrypt it to the volatile RAM on demand. This needs some thoughts about how the decryption password can be entered. Perhaps over BLE from a phone app? Perhaps through an onboard web server?

2. Have a "call home" feature, where the device tries to obtain an internet connection, and then checks whether the device is marked as stolen. If it is, it formats itself. Again, perhas the device only works if it has internet access through your phone through an app, or something.

3. The firmware uses the boot button to trigger the mounting of the drive. That button is really awkward to press. What I'd really like is a nice case that shows the LED, and something like a TP223 touch key to replace pressing the little fiddly button. Those things are sensitive: you'll find a similar touch key on a Yubikey, for example.

## Hardware

Endive targets **ESP32-S3 Super Mini** boards with 4 MB flash and a USB-C connector wired to the chip's native USB pins. These are widely available for $3–4:

[ESP32-S3 Super Mini on AliExpress](https://www.aliexpress.com/item/1005007523988592.html?spm=a2g0o.order_list.order_list_main.11.1d802CVyvzq)

Use a **data-capable** USB-C cable — charge-only cables won't work.

## How it works

At boot the board exposes a **USB Serial/JTAG** port, which is what you use for flashing and monitoring. When you press **BOOT (GPIO0)**, it re-enumerates as a **composite USB device**: a removable FAT volume (backed by wear-leveled internal flash) plus a CDC serial port. The host sees the disk as **ENDIVE**.

The on-board LED tells you what's happening at a glance:

| LED colour | Meaning |
|---|---|
| Red | Drive not connected |
| Green | Drive connected |
| Flashing yellow | Session about to expire. Press BOOT to extend, or just let it disconnect. |

Session length and warning timing are controlled by a `config` file in the root of the volume. Full details on all the timers and eject behaviour are in the header of `main/main.c`.

## Requirements

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/index.html) **v5.x** (the exact supported range is declared in `main/idf_component.yml`)
- Python and CMake, as required by ESP-IDF
- A data-capable USB-C cable and a free USB port

## Building

```bash
cd esp32-s3-mini
idf.py set-target esp32s3
idf.py build
```

The first build will fetch managed components (TinyUSB, LED strip, etc.) from the Espressif component registry automatically.

## Flashing

Put the board into download mode — hold **BOOT**, tap **RESET**, release **BOOT** — then flash and open the monitor:

```bash
idf.py -p PORT flash monitor
```

Replace `PORT` with your serial device (e.g. `/dev/ttyACM0` on Linux). Exit the monitor with **Ctrl+]**.

## Using the drive

1. Let the board boot. The USB port starts in Serial/JTAG mode, ready for flashing.
2. Press **BOOT** to mount **ENDIVE** on the host as a USB drive.
3. Your `.env` symlinks (or files) are now accessible. The session will end automatically when the timer expires; the flashing yellow LED gives you advance warning.
4. To extend the session, press **BOOT** again while the drive is mounted.
5. To end the session early, eject the volume from your OS in the normal way. The firmware detects this and closes the session after a short debounce.

**Adjusting session length:** edit the `config` file in the volume root (`session_seconds`, `warning_seconds`). If the file is missing, the firmware creates one with defaults on first use. After editing `config`, unplug and replug the USB before the next session so the host rescans the volume correctly.

## Repository layout

| Path | Role |
|------|------|
| `main/main.c` | Application: TinyUSB MSC/CDC, FAT, BOOT, LEDs, timers |
| `main/idf_component.yml` | Component manager dependencies |
| `partitions.csv` | Factory app + 2 MB FAT `storage` partition |
| `sdkconfig.defaults` | Flash size, PSRAM, TinyUSB, FAT, mount path `/usb` |

Build outputs and the local `sdkconfig` are excluded by `.gitignore`; board-specific defaults live in `sdkconfig.defaults` and are safe to commit.

## License

MIT. See `LICENSE`. Copyright (c) 2026 Keir Finlow-Bates.  
