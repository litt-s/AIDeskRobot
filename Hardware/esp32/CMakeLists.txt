cmake_minimum_required(VERSION 3.16)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(IDF_TARGET esp32s3 CACHE STRING "ESP-IDF target")

function(robot_local_root name default_path required_file)
    if(NOT DEFINED ${name})
        if(DEFINED ENV{${name}})
            set(${name} "$ENV{${name}}" CACHE PATH "${name}")
        elseif(NOT "${default_path}" STREQUAL "")
            set(${name} "${default_path}" CACHE PATH "${name}")
        else()
            message(FATAL_ERROR
                "${name} is required; set it as an environment variable or CMake cache path")
        endif()
    endif()
    file(TO_CMAKE_PATH "${${name}}" normalized)
    set(${name} "${normalized}" CACHE PATH "${name}" FORCE)
    if(NOT EXISTS "${${name}}/${required_file}")
        message(FATAL_ERROR "${name} is invalid: ${${name}} (missing ${required_file})")
    endif()
    message(STATUS "${name}=${${name}}")
    set(${name} "${${name}}" PARENT_SCOPE)
endfunction()

get_filename_component(_default_vendor "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
robot_local_root(AIUI_VENDOR_ROOT "${_default_vendor}" "include/aiui/aiui_api.h")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(deskemoji_aiui)

if(NOT IDF_VERSION_MAJOR EQUAL 5 OR NOT IDF_VERSION_MINOR EQUAL 5)
    message(FATAL_ERROR "ESP-IDF 5.5.x is required; found ${IDF_VERSION}")
endif()

spiffs_create_partition_image(assets "${CMAKE_CURRENT_LIST_DIR}/assets" FLASH_IN_PROJECT)
