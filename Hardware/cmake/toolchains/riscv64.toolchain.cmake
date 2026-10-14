
set(UNIX TRUE CACHE BOOL "")
SET(CMAKE_SYSTEM_NAME Linux) # this one is important
SET(CMAKE_SYSTEM_VERSION 1)  # this one not so much
SET(PLATFORM riscv64)

set(CROSS_TOOLCHAIN_DIR "/mnt/i/work/customer/meitianchen/riscv64-glibc-gcc-thead_20200702")
set(CROSS_TOOLCHAIN_PATH_PREFIX "${CROSS_TOOLCHAIN_DIR}/bin/riscv64-unknown-linux-gnu-")

set(CROSS_SYS_ROOT "${CROSS_TOOLCHAIN_DIR}/sysroot/")

message(STATUS "Current CROSS_TOOLCHAIN_PATH_PREFIX is => ${CROSS_TOOLCHAIN_PATH_PREFIX}")
if(("x${CROSS_TOOLCHAIN_PATH_PREFIX}" STREQUAL  "x"))
  message(FATAL_ERROR  "you should set CROSS_TOOLCHAIN_PATH_PREFIX first")
endif()

#set compiler location
set(CMAKE_C_COMPILER "${CROSS_TOOLCHAIN_PATH_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${CROSS_TOOLCHAIN_PATH_PREFIX}g++")
set(CMAKE_AR "${CROSS_TOOLCHAIN_PATH_PREFIX}ar")
set(CMAKE_LINKER "${CROSS_TOOLCHAIN_PATH_PREFIX}ld")
set(CMAKE_RANLIB "${CROSS_TOOLCHAIN_PATH_PREFIX}ranlib")
set(CMAKE_NM "${CROSS_TOOLCHAIN_PATH_PREFIX}nm")
set(CMAKE_OBJDUMP "${CROSS_TOOLCHAIN_PATH_PREFIX}objdump")
set(CMAKE_OBJCOPY "${CROSS_TOOLCHAIN_PATH_PREFIX}objcopy")
set(CMAKE_STRIP "${CROSS_TOOLCHAIN_PATH_PREFIX}strip")

set(CROSS_COMMON_FLAGS " --sysroot=${CROSS_SYS_ROOT}")
string(APPEND CMAKE_C_FLAGS          " -fPIC ${CROSS_COMMON_FLAGS}")
string(APPEND CMAKE_CXX_FLAGS        " -fPIC ${CROSS_COMMON_FLAGS}")
string(APPEND CMAKE_EXE_LINKER_FLAGS " -fPIC -fPIE ${CROSS_COMMON_FLAGS}")

# cmake built-in settings to use find_xxx() functions
SET(CMAKE_FIND_ROOT_PATH  ${CROSS_SYS_ROOT})
# search for programs in the build host directories
SET(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# for libraries and headers in the target directories
SET(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
SET(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# sysroot location
#set(MYSYSROOT /path/to/sysroots/cortexa7-vfp-neon-telechips-linux-gnueabi)
#set(MYSYSROOT "/mnt/d/env/rk3308/rk3308-arm64-glibc-2018.03-toolschain")
# compiler/linker flags
# -march=armv7 -pipe -fomit-frame-pointer
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pipe -fomit-frame-pointer" CACHE INTERNAL "" FORCE)
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pipe -fomit-frame-pointer" CACHE INTERNAL "" FORCE)
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --sysroot=${MYSYSROOT}" CACHE INTERNAL "" FORCE)
#set(CMAKE_C_LINK_FLAGS "${CMAKE_C_LINK_FLAGS} --sysroot=${MYSYSROOT}" CACHE INTERNAL "" FORCE)
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --sysroot=${MYSYSROOT}" CACHE INTERNAL "" FORCE)
#set(CMAKE_CXX_LINK_FLAGS "${CMAKE_CXX_LINK_FLAGS} --sysroot=${MYSYSROOT}" CACHE INTERNAL "" FORCE)
# cmake built-in settings to use find_xxx() functions
#set(CMAKE_FIND_ROOT_PATH "${MYSYSROOT}")
#set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
#set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
#set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

