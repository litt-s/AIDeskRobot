cmake_minimum_required(VERSION 3.10)

project(aiui_ws VERSION 1.0.0 LANGUAGES C CXX)
message(STATUS "\n Current PROJECT_VERSION=${PROJECT_VERSION}, build_type=${CMAKE_BUILD_TYPE} \n")

# 添加自定义cmake文件目录
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake ${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchains)
include(CheckSymbolExists)
include(macros)

set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_COLOR_MAKEFILE ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
#set(CMAKE_C_EXTENSIONS FALSE)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

#set(CMAKE_BUILD_TYPE "Debug")

#set(CMAKE_C_COMPILER_INIT g++)
#add_compile_options(-O0 -g -ggdb -Wall)
#add_compile_options(-Wno-unused-result -Wunused-function)
#set(CFLAGS -Wunused-result)
#set(CMAKE_C_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -Wall -Wunused-result -g -ggdb")
#set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -Wall -Wunused-result -g -ggdb")

#set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O0 -Wunused-result -Wall")

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_CURRENT_LIST_DIR}/tool/deploy/bin/)
set(LIBRARY_OUTPUT_PATH ${EXECUTABLE_OUTPUT_PATH})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${EXECUTABLE_OUTPUT_PATH})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${EXECUTABLE_OUTPUT_PATH})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${EXECUTABLE_OUTPUT_PATH})
if (UNIX)
    add_compile_options("-fpermissive")
endif (UNIX)

message(STATUS "Output Dir: " ${LIBRARY_OUTPUT_PATH})

option(ENABLE_PORTAUDIO "ENABLE_PORTAUDIO" OFF)
if(ENABLE_PORTAUDIO)
    add_compile_definitions(ENABLE_PORTAUDIO=1)
endif()
message(STATUS "ENABLE_PORTAUDIO: " ${ENABLE_PORTAUDIO})

if (DEFINED AIUI_APP_ID)
    add_compile_definitions(AIUI_APP_ID=${AIUI_APP_ID}
            AIUI_APP_KEY=${AIUI_APP_KEY}
            AIUI_APP_SECRET=${AIUI_APP_SECRET}
            AIUI_SCENE=${AIUI_SCENE})
endif (DEFINED AIUI_APP_ID)

include_directories(
        "${CMAKE_CURRENT_LIST_DIR}/include"
        "${CMAKE_CURRENT_LIST_DIR}/include/aiui"
        "${CMAKE_CURRENT_LIST_DIR}/src"
        "${CMAKE_CURRENT_LIST_DIR}/src/aiui"
)

#link_directories(${CMAKE_CURRENT_SOURCE_DIR}/lib)
if (ENABLE_PORTAUDIO)
    option(PORTAUDIO_BUILD_SNDIO "Enable SNDIO" OFF)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/src/3rdparty/portaudio)
endif (ENABLE_PORTAUDIO)

# --- 1. add aiui src
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/src/aiui SRC_LIST)
set_source_files_properties(${SRC_LIST} PROPERTIES LANGUAGE C)
add_library(aiui_ws ${SRC_LIST})

# --- 2. add audio_recorder/audio_player
if (ENABLE_PORTAUDIO)
    # recorder
    aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/src/audio_recorder AUDIO_RECORDER_SRC_LIST)
    set_source_files_properties(${AUDIO_RECORDER_SRC_LIST} PROPERTIES LANGUAGE CXX)
    add_library(audio_recorder ${AUDIO_RECORDER_SRC_LIST})
    target_include_directories(audio_recorder PUBLIC
            "${CMAKE_CURRENT_LIST_DIR}/include/audio_recorder"
            "${CMAKE_CURRENT_LIST_DIR}/src/audio_recorder")
    target_link_libraries(audio_recorder PUBLIC portaudio)

    # player
    aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/src/audio_player AUDIO_PLAYER_SRC_LIST)
    set_source_files_properties(${AUDIO_PLAYER_SRC_LIST} PROPERTIES LANGUAGE CXX)
    add_library(audio_player ${AUDIO_PLAYER_SRC_LIST})
    target_include_directories(audio_player PUBLIC
            "${CMAKE_CURRENT_LIST_DIR}/include/audio_player"
            "${CMAKE_CURRENT_LIST_DIR}/src/audio_player")
    target_link_libraries(audio_player PUBLIC portaudio)
endif (ENABLE_PORTAUDIO)

# --- 3. add demo
#aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/src/demo SRC_LIST_DEMO)
if(ENABLE_PORTAUDIO)
    set(SRC_LIST_DEMO ${CMAKE_CURRENT_LIST_DIR}/src/demo/demo.c)
else()
    set(SRC_LIST_DEMO ${CMAKE_CURRENT_LIST_DIR}/src/demo/demo_offline.c)
endif ()
set_source_files_properties(${SRC_LIST_DEMO} PROPERTIES LANGUAGE C)
add_executable(aiui_demo ${SRC_LIST_DEMO})
target_include_directories(aiui_demo PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/demo")

target_link_libraries(aiui_demo PRIVATE aiui_ws pthread dl)
if (ENABLE_PORTAUDIO)
    target_link_libraries(aiui_demo PRIVATE audio_recorder audio_player)
endif (ENABLE_PORTAUDIO)

##target_link_libraries(aiui_demo asound)
#target_link_libraries(aiui_demo pthread)
#target_link_libraries(aiui_demo mbedtls)
#target_link_libraries(aiui_demo mbedx509)
#target_link_libraries(aiui_demo mbedcrypto)

