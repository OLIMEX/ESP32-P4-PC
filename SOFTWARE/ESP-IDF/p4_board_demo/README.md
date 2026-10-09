# ESP32-P4-PC board demo and interactive hardware check

This ESP-IDF project is a customer-facing demonstration of the main onboard
peripherals of the OLIMEX ESP32-P4-PC. It is separate from
`p4_production_test`; building or changing this demo does not modify the
production-test application.

The demo shows board and firmware information on HDMI and then guides the user
through five tests:

1. HDMI output and the onboard LT8912B DSI-to-HDMI bridge
2. The native four-bit SDMMC microSD interface
3. USB host and a USB mass-storage device
4. The ES8311 microphone-to-headphone audio path
5. The IP101 10/100 Ethernet interface

At the end, the HDMI display and serial monitor show a PASS/WARN/FAIL/SKIP
summary. No factory fixture is required; the BOOT button is the only control.

## Choose how to use the demo

There are two supported paths:

1. **Flash the ready-made merged binary** if you only want to run the hardware
   check. This is the quickest path and does not require compiling the project.
2. **Build from source** if you want to inspect the code, change configuration,
   or regenerate the firmware. The validated toolchain is ESP-IDF v5.5.5.

The supplied image and the default source configuration both target an
ESP32-P4-PC with 16 MB flash and 32 MB PSRAM and use HDMI at 1280x720, 60 Hz.

## Important behavior

- HDMI runs at **1280x720, 60 Hz, RGB888** in the default configuration. It
  preserves the 16:9 aspect ratio of Full HD displays and scales uniformly to
  1920x1080. The corrected timing uses an exact 80 MHz DPI clock.
- The microSD interface runs in native **4-bit SDMMC mode at 20 MHz**. The
  earlier 400 kHz setting is suitable for card initialization or signal-debug
  probing, but it is not a useful normal operating speed.
- The SD and USB tests briefly create `P4DEMO.TXT`, read it back, verify it, and
  remove it. An interrupted or failed test can leave that file behind. Back up
  valuable media before testing.
- Ethernet link and DHCP are the hardware test. The external ping is an
  additional network check and is reported as WARN, not FAIL, if the network
  blocks ICMP or has no Internet connection.
- HDMI and the external MIPI-DSI connector share the same ESP32-P4 DSI lanes.
  This demo selects the onboard HDMI bridge; it does not drive an external DSI
  panel simultaneously.
- HDMI is video-only on this board. Use the 3.5 mm jack for the audio test.

## Option A: flash the supplied binary

The `binary` directory contains one complete merged image:

```text
binary/ESP32-P4-PC-board-demo.bin
```

It contains the bootloader, partition table, and application and must be
written at flash address **0x0**. Do not write this merged file at `0x10000`.

The image was built with ESP-IDF v5.5.5 and esptool v4.12.0. Its SHA-256 is
also recorded in `binary/SHA256SUMS.txt`.

### Flash from an ESP-IDF terminal

Connect the board's USB-C programming/power connector with a data-capable
cable. Close any serial monitor that is already using the port, then replace
`COM51` with the board's actual port and run from this project directory:

```powershell
python -m esptool --chip esp32p4 -p COM51 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 40m --flash_size 16MB 0x0 binary\ESP32-P4-PC-board-demo.bin
```

The ESP32-P4-PC enters download mode and resets automatically. No manual BOOT
or RESET button sequence is required.

### Flash with standalone Python

If ESP-IDF is not installed, install the same esptool release and run the same
write command with `py` on Windows or `python3` on Linux/macOS:

```powershell
py -m pip install esptool==4.12.0
py -m esptool --chip esp32p4 -p COM51 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 40m --flash_size 16MB 0x0 binary\ESP32-P4-PC-board-demo.bin
```

The HDMI interface is sufficient to operate the test. For optional serial
diagnostics, open the same port at **115200 baud, 8 data bits, no parity, one
stop bit** after flashing. Only one program may hold the serial port at a time.

## Source dependencies

The project is self-contained: it does not require the neighboring
`p4_production_test` directory. The `components/esp-bsp` directory contains a
trimmed ESP-BSP snapshot with only the six component trees needed by this
board. Component manifests and license files are retained. Other managed
dependencies are downloaded by the ESP-IDF Component Manager during the first
build.

## Required equipment

- ESP32-P4-PC board
- A stable 5 V USB-C supply and USB-C data cable
- HDMI cable and monitor capable of 1280x720 at 60 Hz
- FAT/FAT32-formatted microSD card
- FAT/FAT32-formatted USB flash drive
- A four-pole 3.5 mm CTIA headset with microphone
- Ethernet cable connected to a network with DHCP
- Computer with a USB port and esptool v4.12.0 or newer
- ESP-IDF v5.5.5 when rebuilding from source

The removable media, headset, and Ethernet connection are optional. Hold
BOOT to skip a test when its accessory is not available.

## Option B: build and flash from source

The final firmware was built and hardware-tested with **ESP-IDF v5.5.5**. The
component manifest accepts `>=5.5.3,<5.6.0`, but v5.5.5 is recommended for a
reproducible build. ESP-IDF v5.5.0 through v5.5.2 must not be used because they
predate required ESP32-P4 clock/frequency fixes and APIs. The USB Host MSC
component is pinned by the project manifest.

1. Open an ESP-IDF terminal.

   On Windows, use **ESP-IDF PowerShell** or **ESP-IDF Command Prompt** for
   ESP-IDF v5.5.5 from the Start menu. On Linux/macOS, source the
   matching ESP-IDF `export.sh` script. Confirm the active version before
   configuring the project:

   ```sh
   idf.py --version
   ```

   The validated output is:

   ```text
   ESP-IDF v5.5.5
   ```

2. Change to the demo directory:

   ```sh
   cd SOFTWARE/ESP-IDF/p4_board_demo
   ```

3. On the first build, select the ESP32-P4 target. This creates `sdkconfig`
   from the complete board settings in `sdkconfig.defaults`:

   ```sh
   idf.py set-target esp32p4
   ```

4. Build the project:

   ```sh
   idf.py build
   ```

5. Connect the board's USB-C connector. Find its serial port and flash it,
   replacing `COM7` with the correct Windows port or using a path such as
   `/dev/ttyACM0` on Linux:

   ```sh
   idf.py -p COM7 flash monitor
   ```

6. No manual download-mode button sequence is needed on ESP32-P4-PC. The USB
   serial/JTAG interface resets the board and starts the ROM downloader
   automatically.

7. Exit the ESP-IDF serial monitor with `Ctrl+]` before running another flash
   command. Otherwise the port remains busy and esptool reports `Access is
   denied` or `port is busy`.

### Validated build details

| Item | Value |
| --- | --- |
| Target | ESP32-P4 |
| Tested silicon | revision v1.3 |
| ESP-IDF | v5.5.5 |
| Flash | 16 MB, DIO, 40 MHz |
| PSRAM | 32 MB, 200 MHz |
| Bootloader offset | `0x2000` |
| Partition-table offset | `0x8000` |
| Factory application offset | `0x10000` |
| Factory partition size | `0x400000` (4 MB) |
| Final application size | `0x126430` bytes (71% of partition free) |
| Serial console | 115200 baud |
| HDMI | 1280x720 at 60 Hz, RGB888 |

The application-only file produced by `idf.py build` is
`build/esp32_p4_pc_board_demo.bin`; it is not interchangeable with the merged
file in `binary`. The application-only file uses offset `0x10000`, while the
supplied merged image uses offset `0x0`.

For a completely clean rebuild:

```sh
idf.py fullclean
idf.py set-target esp32p4
idf.py build
```

Do not copy an `sdkconfig` from a different ESP32-P4 board. The GPIO assignments
for SDMMC, Ethernet, audio, HDMI, and the BOOT button are board-specific and are
already recorded in this project's `sdkconfig.defaults`.

### Regenerate the merged release image

After a successful build, maintainers can recreate the single-file image with:

```powershell
python -m esptool --chip esp32p4 merge_bin --output binary\ESP32-P4-PC-board-demo.bin --format raw --target-offset 0x0 --flash_mode dio --flash_freq 40m --flash_size 16MB 0x2000 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\esp32_p4_pc_board_demo.bin
Get-FileHash -Algorithm SHA256 binary\ESP32-P4-PC-board-demo.bin
```

Update `binary/SHA256SUMS.txt` whenever the image changes.

## Button controls

The test screens always print the available action. In general:

- **Short press BOOT:** begin, accept, or continue
- **Hold BOOT for two seconds:** skip a test or mark a manual test as failed

During the Ethernet test GPIO35 is used as RMII TXD1, so the BOOT button is
temporarily unavailable. It is restored for the result screen. Ethernet is
kept initialized after the successful check to avoid a harmless ESP-IDF 5.5
EMAC/IGMP teardown error; choosing “run again” performs a clean software reset.

## Step-by-step test procedure

### 1. Startup information

After reset, the HDMI screen should report:

- ESP-IDF version
- ESP32-P4 silicon revision and core count
- detected flash and PSRAM sizes
- `1280x720 @ 60 Hz` HDMI mode
- `4-bit @ 20 MHz` SDMMC mode

Check that the expected 16 MB flash and 32 MB PSRAM are detected. Short-press
BOOT to begin. A long press leaves this information screen displayed until the
board is reset.

### 2. HDMI visual test

1. Connect HDMI before powering or resetting the board.
2. Short-press BOOT at the startup screen.
3. Observe the color bars for five seconds.
4. Confirm that the monitor remains synchronized and that the bars are stable,
   correctly colored, and free of flicker, tearing, repeated lines, or noise.
5. Short-press BOOT for PASS, or hold it for two seconds for FAIL.

This checks the ESP32-P4 DSI/DPI output path, LT8912B bridge initialization,
HDMI timing, RGB data, and sustained framebuffer output. It cannot prove every
monitor is compatible, so monitor model and cable quality remain useful
diagnostic information.

### 3. MicroSD/SDMMC test

1. Use a card whose contents are backed up and formatted as FAT/FAT32.
2. Insert it into the microSD socket before starting this stage.
3. Short-press BOOT to run, or hold BOOT to skip.
4. Wait while the demo powers the socket, initializes the ESP32-P4 SDMMC host
   in 4-bit mode at 20 MHz, and mounts the filesystem.
5. The demo creates `P4DEMO.TXT`, writes a known string, flushes it, reads it
   back, compares it byte-for-byte, and removes it.
6. Read the on-screen result and short-press BOOT to continue.

A mount failure can indicate an unsupported filesystem, a damaged card, poor
contact, or SDMMC signal/power trouble. A write/read/verify failure means the
filesystem mounted but the data transaction was not reliable.

### 4. USB host mass-storage test

1. Use a backed-up USB flash drive with a **FAT or FAT32 partition**. Both MBR
   and GPT partition tables are supported; exFAT and NTFS are not. Windows
   showing “File system: FAT32” in drive Properties is a suitable check.
2. Insert it into one of the four USB host connectors. Do not use the USB-C
   programming/power connector; that connector is not a host port.
3. Short-press BOOT to run, or hold BOOT to skip.
4. The demo resets the onboard USB hub and waits up to 30 seconds for an MSC
   device.
5. It mounts the drive, creates and verifies `P4DEMO.TXT`, removes the file,
   unmounts the drive, and releases the host stack.
6. Read the result and short-press BOOT to continue.

To check all physical sockets, run the whole demo again with the drive inserted
in a different USB host connector. One successful run proves the hub and the
selected downstream port, not all four connectors.

### 5. Audio loopback test

1. Reduce the volume, then connect ordinary headphones or a four-pole CTIA
   headset to the board's 3.5 mm jack.
2. Short-press BOOT to start the output test, or hold BOOT to skip audio.
3. Confirm that the steady 1 kHz tone is clear, then short-press BOOT. Hold the
   button instead if the tone is absent or distorted.
4. The microphone-input stage now offers two choices:
   - With a four-pole CTIA headset, short-press BOOT, speak into or tap gently
     on its microphone, and confirm that the sound is heard through the output.
   - With ordinary three-pole headphones, hold BOOT to skip the microphone
     stage. The result is WARN: headphone output passed, microphone untested.
5. During microphone loopback, short-press BOOT for PASS, or hold it for two
   seconds for FAIL. The firmware also requires a measured microphone peak, so
   an accidental short press with ordinary headphones cannot produce a false
   full PASS.

The board has no onboard microphone. A full audio PASS therefore requires a
four-pole CTIA headset with a microphone; ordinary headphones can validate only
the output path.

The board uses an ES8311 mono codec. The production test therefore keeps both
I2S directions in the codec's native one-channel format. A short startup pop is
suppressed before live microphone samples are sent to the output.

### 6. Ethernet test

1. Connect Ethernet to a switch/router that provides DHCP.
2. Short-press BOOT to run, or hold BOOT to skip.
3. The demo releases GPIO35 from the BOOT-button function and starts the IP101
   PHY and ESP32-P4 RMII MAC.
4. It waits for physical link, then for a DHCP address.
5. After obtaining an address, it sends ten pings to the configured target,
   `8.8.8.8` by default.

Result meanings:

- **PASS:** link, DHCP, and the external ping succeeded.
- **WARN:** link and DHCP succeeded, but the ping target was unavailable.
- **FAIL:** physical link or DHCP failed.

The external target can be changed with `idf.py menuconfig` under **LAN Test →
Optional ping target IPv4 address**. A local, known-reachable test server is a
better choice than an Internet address on restricted networks.

### 7. Summary and repeat

The final screen lists every subsystem and its result. The same information is
printed to the serial monitor between `DEMO_RESULT_BEGIN` and
`DEMO_RESULT_END`, making it easy to copy into a support report.

- Short-press BOOT to restart the board and repeat all stages from a clean
  hardware state.
- Hold BOOT for two seconds to keep the summary displayed until reset.

## Troubleshooting

### The serial port is busy

Exit any running ESP-IDF monitor with `Ctrl+]` and close other terminal
programs using the port. A previous monitor process holding `COM51` produces
`PermissionError(13, 'Access is denied')`; it is not a flash or board fault.

### The board briefly reports `waiting for download`

The USB serial/JTAG port can disconnect and reconnect while esptool resets the
chip. Let the monitor reconnect. A successful application boot continues with
`Starting ESP32-P4-PC board demo`.

### HDMI is blank, unstable, or repeatedly resynchronizes

- Connect HDMI before powering or resetting the board.
- Use a known-good cable and a display that accepts 1280x720 at 60 Hz.
- Confirm the serial log contains `HDMI configuration for 1280x720@60HZ` and
  `Display resolution 1280x720`.
- The final 720p mode is preferred over the earlier 800x600 experiment on Full
  HD displays because it has the correct 16:9 aspect ratio and scales exactly
  by 1.5 to 1920x1080.

### The microSD test fails

Use FAT/FAT32, reseat the card, and try another known-good card. The normal test
is four-bit SDMMC at 20 MHz; passing only at a much lower debug clock does not
count as a normal-speed pass.

### The USB drive is detected but will not mount

Reformat its partition as FAT or FAT32. exFAT and NTFS are unsupported. The
firmware supports both MBR and GPT media; GPT drives are mapped to their FAT
partition before mounting. Use one of the four USB host connectors, not the
USB-C programming/power connector.

### Audio produces only a click or no sustained tone

Ensure headphones are connected to the 3.5 mm jack before starting the audio
stage. HDMI does not carry audio. Ordinary three-pole headphones can test the
1 kHz output tone but have no microphone, so hold BOOT at the microphone stage
to record `WARN - headphone output passed; microphone input skipped`. A full
audio PASS requires a four-pole CTIA headset with microphone.

### Ethernet passes but Internet ping does not

Link and DHCP prove the board-level Ethernet path. Networks may block ICMP or
Internet access; in that case the result is WARN. Configure a reachable local
IPv4 target under **LAN Test** in `idf.py menuconfig` if required.

### Overall result is WARN

WARN is expected when an optional part was skipped—for example, using ordinary
headphones and skipping the microphone check. Review each summary line; WARN
does not change a separate HDMI, storage, or Ethernet PASS.

## Result interpretation

- **PASS:** the requested functional check completed successfully.
- **WARN:** the board-level portion passed, but an external condition such as
  Internet ping could not be confirmed.
- **FAIL:** initialization, data verification, or manual inspection failed.
- **SKIP:** the operator chose not to run the stage; it is not a pass.

This is an interactive demonstration and diagnostic aid, not a replacement for
the fixture-driven manufacturing test. It does not test MIPI-CSI cameras,
external MIPI-DSI panels, UEXT/GPIO wiring, LiPo charging, battery switchover,
or the optional PoE module. Those functions require accessories or measurement
fixtures and are best kept as separate examples or production-test stages.

## Configuration notes

Run `idf.py menuconfig` only when a different diagnostic setup is required.
Useful settings are located under:

- **Board Support Package → Display** for the HDMI mode
- **SD Card Test** for SDMMC clock and bus parameters
- **Audio Loopback Test** for codec/I2S parameters
- **LAN Test** for PHY pins, DHCP timeout, and ping target
- **PT Display / HDMI test** for the BOOT-button long-press time

The checked-in defaults are the known ESP32-P4-PC values. If SD debugging is
necessary, 10 MHz and then 400 kHz can be tried temporarily. A card that works
only at 400 kHz should not be considered a successful normal-speed test.
