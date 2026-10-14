cmake_minimum_required(VERSION 3.16)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(IDF_TARGET esp32s3 CACHE STRING "ESP-IDF target")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(deskemoji_robot)

if(NOT IDF_VERSION_MAJOR EQUAL 5 OR NOT IDF_VERSION_MINOR EQUAL 5)
    message(FATAL_ERROR "ESP-IDF 5.5.x is required; found ${IDF_VERSION}")
endif()

spiffs_create_partition_image(assets "${CMAKE_CURRENT_LIST_DIR}/assets" FLASH_IN_PROJECT)
