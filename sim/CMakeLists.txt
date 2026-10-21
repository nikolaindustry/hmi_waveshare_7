# Desktop (SDL2) simulator for the Hyperwisor HMI UI.
#
# Compiles the REAL LVGL sources and the REAL components/app/ui sources,
# with sim/stubs/ standing in for ESP-IDF. Nothing under components/ is
# modified or copied -- the UI you preview here is the UI that ships.
#
#   cmake -S sim -B sim/build && cmake --build sim/build && ./sim/build/hmi_sim
# or just: ./sim/run.sh

cmake_minimum_required(VERSION 3.16)
project(hmi_sim C)

set(CMAKE_C_STANDARD 11)
set(REPO ${CMAKE_CURRENT_LIST_DIR}/..)

find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    # Homebrew on Apple Silicon doesn't ship an SDL2 .pc/config in the
    # default search path; fall back to the known prefix.
    set(SDL2_INCLUDE_DIRS /opt/homebrew/include)
    set(SDL2_LIBRARIES    -L/opt/homebrew/lib -lSDL2)
endif()

# ---- LVGL (vendored in components/lvgl) ----
file(GLOB_RECURSE LVGL_SRCS ${REPO}/components/lvgl/src/*.c)

# ---- The application UI, compiled from its real location ----
set(UI_DIR ${REPO}/components/app/ui)
set(APP_SRCS
    ${UI_DIR}/ui_shell.c
    ${UI_DIR}/ui_theme.c
    ${UI_DIR}/ui_tab_overview.c
    ${UI_DIR}/ui_tab_control.c
    ${UI_DIR}/ui_tab_climate.c
    ${UI_DIR}/ui_tab_entertainment.c
    ${UI_DIR}/ui_tab_maintenance.c
    ${UI_DIR}/ctrl_state.c
    ${UI_DIR}/ac_state.c
    ${UI_DIR}/owner_name.c
    ${UI_DIR}/tile_names.c
    ${UI_DIR}/display_brightness.c
    ${UI_DIR}/splash.c
    ${UI_DIR}/img_nikola_logo.c
    ${UI_DIR}/logo_chintamani.c
    ${UI_DIR}/logo_splash.c
    ${UI_DIR}/van_hero_img.c
    ${UI_DIR}/car_hero_img.c
    ${UI_DIR}/vent_img.c
    ${UI_DIR}/font_cursive_40.c
    ${UI_DIR}/font_cursive_56.c
    # Real settings modules -- they run against the in-memory NVS stub.
    ${REPO}/components/app/hmi_role/hmi_role.c
    ${REPO}/components/app/bus_config/bus_config.c
)

# ---- Declarative dashboard renderer (components/app/hds) ----
# Compiled from its real location, same as the UI above. cJSON comes
# from the managed component the firmware already pulls in, so the sim
# parses specs with the exact parser that runs on the device.
set(HDS_DIR ${REPO}/components/app/hds)
set(HDS_SRCS
    ${HDS_DIR}/hds_spec.c
    ${HDS_DIR}/hds_inject.c
    ${HDS_DIR}/hds_assets.c
    ${HDS_DIR}/hds_widget.c
    ${HDS_DIR}/hds_render.c
)
set(CJSON_DIR ${REPO}/managed_components/espressif__cjson/cJSON)

add_executable(hmi_sim
    ${CMAKE_CURRENT_LIST_DIR}/main.c
    ${CMAKE_CURRENT_LIST_DIR}/stubs/stubs.c
    ${APP_SRCS}
    ${HDS_SRCS}
    ${CJSON_DIR}/cJSON.c
    ${LVGL_SRCS}
)

target_include_directories(hmi_sim PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/stubs/include   # must precede the real dirs
    ${CMAKE_CURRENT_LIST_DIR}                 # lv_conf.h
    ${REPO}/components/lvgl
    ${UI_DIR}
    ${REPO}/components/app/include
    ${REPO}/components/app/modbus
    ${REPO}/components/app/hvac
    ${REPO}/components/app/sensor
    ${REPO}/components/app/hmi_role
    ${REPO}/components/app/hmi_sync
    ${REPO}/components/app/hmi_link
    ${REPO}/components/app/bus_config
    ${REPO}/components/bsp_s3_rgb/include
    ${HDS_DIR}
    ${CJSON_DIR}
    ${SDL2_INCLUDE_DIRS}
)

target_compile_definitions(hmi_sim PRIVATE LV_CONF_INCLUDE_SIMPLE)
target_compile_options(hmi_sim PRIVATE -Wall -Wno-unused-parameter)
target_link_libraries(hmi_sim PRIVATE ${SDL2_LIBRARIES} m)
