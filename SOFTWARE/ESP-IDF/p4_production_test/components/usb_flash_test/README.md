usb_flash_test (component)
=========================

- USB Host MSC + FATFS
- Монтира флашка на /usb
- Пише /usb/USBTEST.TXT, чете го обратно и проверява съдържанието

API:
- usb_flash_test_init()
- usb_flash_test_run(timeout_ms, out, out_len)
- usb_flash_test_deinit()

Зависимост:
- espressif/usb_host_msc (managed component)
