# Dolos - a safety-gated USB-HID (BadUSB) payload runner for the ESP32-S3-GEEK.
#
# FOR AUTHORIZED LAB / EDUCATIONAL USE ONLY. Boots SAFE, never types until it is
# armed by hand at the device. The DuckyScript engine is pure C and unit-tested
# with NO ESP-IDF at all - see test/host (make). Build for hardware with v5.5+:
#   . $IDF_PATH/export.sh && idf.py set-target esp32s3 && idf.py build
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dolos)
