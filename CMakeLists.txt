# ESP32-S3 CYBER WEB RADIO (E-Ink) - top-level project file
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Shown on the dashboard firmware card and embedded in the OTA image header.
set(PROJECT_VER "1.1.0")
project(esp32s3_radio_eink)
