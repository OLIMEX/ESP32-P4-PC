# USB flash production-test component

Uses the ESP-IDF USB Host MSC and FATFS layers. It waits for a FAT/FAT32-formatted USB
mass-storage device, mounts it at `/usb`, writes `/usb/P4DEMO.TXT`, reads the
file back, verifies its contents, and removes the test file. The device is
briefly modified while the test is running; an interrupted test can leave the
file behind.

The ESP32-P4-PC's four USB-A sockets are all connected through its onboard
FE1.1s hub. Any of the four sockets can be used for this test; ESP-IDF external
hub support must therefore remain enabled in the project configuration.

ESP-IDF 5.5 does not enable exFAT support in its bundled FatFs configuration.
Use FAT16 or FAT32; exFAT and NTFS drives will be detected by USB but will not
mount. This project includes a local USB MSC adapter that supports FAT/FAT32
drives using either an MBR or GPT partition table. Stock ESP-IDF 5.5 FatFs
normally compiles out GPT discovery.

## API

- `usb_flash_test_init()` powers/resets the board's USB hub and starts USB host.
- `usb_flash_test_run(timeout_ms, out, out_len)` waits for and tests the drive.
- `usb_flash_test_deinit()` unmounts the drive and releases host resources.

The component depends on the pinned `espressif/usb_host_msc` managed component.
