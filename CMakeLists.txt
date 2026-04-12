cmake_minimum_required(VERSION 3.24)

set(IDF_TARGET "esp32s3")
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(PRINTSPHERE_RELEASE_VERSION "v1.4.2-debug")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(printsphere_idf)

# Work around an Xtensa GCC ICE seen in ESP-IDF 5.5.2 while compiling
# esp_lcd_panel_rgb.c with the default debug/-Og flags on Windows.
idf_component_get_property(ESP_LCD_DIR esp_lcd COMPONENT_DIR)
if(ESP_LCD_DIR)
    set_source_files_properties(
        "${ESP_LCD_DIR}/rgb/esp_lcd_panel_rgb.c"
        PROPERTIES
            COMPILE_OPTIONS "-O0"
    )
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(RELEASE_INITIAL_FLASH_DIR "${CMAKE_SOURCE_DIR}/release")
set(RELEASE_INITIAL_FLASH_IMAGE "${RELEASE_INITIAL_FLASH_DIR}/firmware.bin")
set(RELEASE_INITIAL_FLASH_VERSIONED_IMAGE
    "${RELEASE_INITIAL_FLASH_DIR}/firmware-${PRINTSPHERE_RELEASE_VERSION}.bin")
set(RELEASE_INITIAL_FLASH_PACKAGER "${CMAKE_SOURCE_DIR}/tools/package_initial_flash.py")
# Error lookup TSV is embedded directly into the app binary via EMBED_TXTFILES
# in main/CMakeLists.txt — no SPIFFS partition needed.

add_custom_target(
    release_initial_flash
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RELEASE_INITIAL_FLASH_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${RELEASE_INITIAL_FLASH_PACKAGER}"
            --build-dir "${CMAKE_BINARY_DIR}"
            --output "${RELEASE_INITIAL_FLASH_IMAGE}"
            --version "${PRINTSPHERE_RELEASE_VERSION}"
    BYPRODUCTS
            "${RELEASE_INITIAL_FLASH_IMAGE}"
            "${RELEASE_INITIAL_FLASH_VERSIONED_IMAGE}"
    DEPENDS
            app
            bootloader
            "${CMAKE_BINARY_DIR}/flasher_args.json"
            "${RELEASE_INITIAL_FLASH_PACKAGER}"
    COMMENT "Packaging initial flash image at ${RELEASE_INITIAL_FLASH_IMAGE}"
    VERBATIM
)
