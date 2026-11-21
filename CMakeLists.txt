# MoInk BLE firmware - native ESP-IDF project (idf.py, ESP-IDF 5.2.2).
#
# ASCII ONLY in this file: CMake/kconfgen on a Chinese Windows read build inputs
# with the GBK code page, so non-ASCII comments can abort the build.
#
# Sources physically live in src/ (the tree started as a PlatformIO layout);
# main/CMakeLists.txt registers src/*.c as the IDF `main` component, so no file
# has to move.
#
#   idf.py build
#   idf.py -p COM9 flash monitor
#
# The first flash must go over USB (the partition table is incompatible with the
# R series). After that, BLE OTA is enough.

cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "B2.0.1")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(moink_ble)
