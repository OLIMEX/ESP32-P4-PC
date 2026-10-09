idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp32_p4_function_ev_board
        pt_display
        lvgl
        esp_lcd
        esp_timer
        esp_psram
        spi_flash
        sd_card_test
        usb_flash_test
        audio_loopback_test
        lan_test
)
