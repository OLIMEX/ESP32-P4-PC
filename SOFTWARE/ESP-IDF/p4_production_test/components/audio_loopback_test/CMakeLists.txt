idf_component_register(
    SRCS "src/audio_loopback_test.c"
    INCLUDE_DIRS "include"
    REQUIRES esp32_p4_function_ev_board esp_codec_dev
)
