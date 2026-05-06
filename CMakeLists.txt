# Top-level CMakeLists for hyperwisor_s3
# Target: Waveshare ESP32-S3-Touch-LCD-7 (ESP32-S3-N16R8)
# Requires: ESP-IDF v5.5.x
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/components
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hyperwisor_s3)
