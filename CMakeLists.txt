cmake_minimum_required(VERSION 3.16)

# BTstack lives outside this project tree. Point BTSTACK_ROOT at the checkout so
# its ESP32 port components (btstack, btstack_config) can be resolved. Override by
# setting the BTSTACK_ROOT environment variable before building.
if(NOT DEFINED ENV{BTSTACK_ROOT})
    set(ENV{BTSTACK_ROOT} "C:/btstack")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include($ENV{BTSTACK_ROOT}/port/esp32/components/btstack/cmake/BTstack_project.cmake)

project(esp32_spotify_controller)