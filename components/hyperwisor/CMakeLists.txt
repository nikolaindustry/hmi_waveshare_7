set(_srcs
    "hyperwisor_core.c"
    "hyperwisor_wifi.c"
    "hyperwisor_ws.c"
    "hyperwisor_cmd.c"
    "hyperwisor_nvs.c"
    "hyperwisor_http.c"
    "hyperwisor_ntp.c"
    "hyperwisor_widget.c"
    "hyperwisor_port.c"
)

# --- Feature-gated source files ---
# When a feature is disabled, its source file is excluded from the build.
# REQUIRES stay unconditional to avoid CMake cache invalidation issues
# when toggling features. The linker strips unused code anyway.

if(CONFIG_HYPERWISOR_ENABLE_OTA)
    list(APPEND _srcs "hyperwisor_ota.c")
endif()

if(CONFIG_HYPERWISOR_ENABLE_GPIO)
    list(APPEND _srcs "hyperwisor_gpio.c")
endif()

# --- Conditional REQUIRES (only for features that pull in heavy deps) ---

set(_requires
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_http_client
    esp_http_server
    esp_timer
    espressif__cjson
    esp_https_ota
    app_update
    driver
)

set(_priv_requires
    esp_websocket_client
    esp-tls
)

# --- Build-time partition table validation for OTA ---
# If OTA is enabled but the partition table has no ota_0/ota_1 slots,
# the firmware will flash fine but esp_https_ota() will fail at runtime
# with a confusing error. Catch this early with a clear message.

if(CONFIG_HYPERWISOR_ENABLE_OTA)
    if(NOT CONFIG_PARTITION_TABLE_CUSTOM)
        message(WARNING
            "Hyperwisor OTA is enabled but you are not using a custom "
            "partition table. OTA requires ota_0/ota_1 slots. Copy "
            "partitions_XXmb_ota.csv from the hyperwisor component and add:\n"
            "  CONFIG_PARTITION_TABLE_CUSTOM=y\n"
            "  CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"partitions_16mb_ota.csv\"")
    endif()
endif()

idf_component_register(
    SRCS ${_srcs}
    INCLUDE_DIRS "."
    REQUIRES ${_requires}
    PRIV_REQUIRES ${_priv_requires}
)
