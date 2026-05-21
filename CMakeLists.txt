cmake_minimum_required(VERSION 3.24)

set(IDF_TARGET "esp32s3")
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(STATUS_SPHERE_RELEASE_VERSION "v1.0.0")
set(PROJECT_VER "${STATUS_SPHERE_RELEASE_VERSION}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(status_sphere_idf)

idf_component_get_property(ESP_LCD_DIR esp_lcd COMPONENT_DIR)
idf_component_get_property(ESP_LCD_LIB esp_lcd COMPONENT_LIB)
if(ESP_LCD_DIR AND ESP_LCD_LIB)
    set_source_files_properties(
        "${ESP_LCD_DIR}/rgb/esp_lcd_panel_rgb.c"
        TARGET_DIRECTORY ${ESP_LCD_LIB}
        PROPERTIES
            COMPILE_OPTIONS "-O0"
    )
endif()
