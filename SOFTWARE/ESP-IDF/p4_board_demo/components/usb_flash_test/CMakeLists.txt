idf_component_register(
    SRCS "src/usb_flash_test.c"
    INCLUDE_DIRS "include"
    REQUIRES usb usb_host_msc fatfs vfs esp_common
)
