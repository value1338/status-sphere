idf_component_register(
    SRCS
        "app_main.cpp"
        "src/application.cpp"
        "src/config_store.cpp"
        "include/font/dosis_20.c"
        "include/font/dosis_32.c"
        "include/font/dosis_40.c"
        "include/font/mdi_30.c"
        "include/font/mdi_40.c"
        "src/layout_renderer.cpp"
        "src/msa2_status.cpp"
        "src/msa2_status_client.cpp"
        "src/pmu.cpp"
        "src/setup_portal.cpp"
        "src/time_sync.cpp"
        "src/ui.cpp"
        "src/wifi_manager.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        XPowersLib
        esp32_s3_touch_amoled_1_75
        esp_driver_gpio
        esp_event
        esp_http_client
        esp_http_server
        esp_netif
        esp_pm
        esp_timer
        lwip
        esp_wifi
        lvgl
        nvs_flash
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    STATUS_SPHERE_RELEASE_VERSION="${STATUS_SPHERE_RELEASE_VERSION}"
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)

set(LAYOUT_JSON "${CMAKE_CURRENT_SOURCE_DIR}/resources/layout.json")
if(EXISTS "${LAYOUT_JSON}")
    target_add_binary_data(${COMPONENT_LIB} "${LAYOUT_JSON}" TEXT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HAS_EDITOR_LAYOUT=1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${LAYOUT_JSON}")
    message(STATUS "Editor layout found: ${LAYOUT_JSON}")
else()
    message(STATUS "No editor layout - using default firmware layout")
endif()
