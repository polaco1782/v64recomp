# Debian-style cross toolchain. Point CMAKE_SYSROOT at a prepared target sysroot
# when cross-building libraries; native Pi builds do not need this file.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
find_program(CMAKE_C_COMPILER NAMES aarch64-linux-gnu-gcc REQUIRED)
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a53")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
