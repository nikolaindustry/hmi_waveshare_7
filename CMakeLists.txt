# Top-level CMakeLists for hyperwisor_s3
# Target: Waveshare ESP32-S3-Touch-LCD-7 (ESP32-S3-N16R8)
# Requires: ESP-IDF v5.5.x
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/components
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hyperwisor_s3)

# ---- Dashboard spec -> the `storage` SPIFFS partition ----------------
# assets/dashboard/ is produced by tools/hds_transcode.py from a
# Hyperwisor dashboard export. When it holds a dashboard.json the app
# renders it instead of the hand-built shell (see components/app/app.c);
# when the directory is empty the partition is still created and the
# shell comes up as before.
#
# FLASH_IN_PROJECT means `idf.py flash` writes it along with the app, so
# re-running the transcoder and reflashing is one step.
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/assets/dashboard/dashboard.json)
    spiffs_create_partition_image(storage
                                  ${CMAKE_CURRENT_LIST_DIR}/assets/dashboard
                                  FLASH_IN_PROJECT)
    message(STATUS "dashboard: assets/dashboard -> storage partition")
else()
    message(STATUS "dashboard: no spec in assets/dashboard; shell UI only")
endif()
