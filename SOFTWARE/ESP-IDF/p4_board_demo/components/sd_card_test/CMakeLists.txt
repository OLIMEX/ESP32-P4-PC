idf_component_register(
    SRCS "src/sd_card_test.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_sdmmc esp_driver_gpio fatfs vfs
)
