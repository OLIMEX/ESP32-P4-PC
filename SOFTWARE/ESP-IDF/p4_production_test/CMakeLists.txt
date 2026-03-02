cmake_minimum_required(VERSION 3.16)

# IMPORTANT:
# Point EXTRA_COMPONENT_DIRS to the *single* BSP component directory, NOT esp-bsp/bsp/
# otherwise IDF will try to parse all BSPs (some are for other targets and will fail).
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/esp-bsp/bsp/esp32_p4_function_ev_board"
    "${CMAKE_CURRENT_LIST_DIR}/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4_hdmi_stable_video)
