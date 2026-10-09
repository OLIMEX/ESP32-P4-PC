cmake_minimum_required(VERSION 3.16)

# Use the corrected ESP32-P4 HDMI BSP snapshot vendored with this project so a
# checkout of p4_board_demo builds without any sibling project dependencies.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/esp-bsp/bsp/esp32_p4_function_ev_board"
    "${CMAKE_CURRENT_LIST_DIR}/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_p4_pc_board_demo)

# esp_video still uses deprecated ESP-IDF 5.5 enum aliases internally. Keep
# managed dependency sources untouched and silence deprecations for that target
# only; warnings in this project's own code remain enabled.
idf_component_get_property(esp_video_lib espressif__esp_video COMPONENT_LIB)
target_compile_options(${esp_video_lib} PRIVATE -Wno-deprecated-declarations)
