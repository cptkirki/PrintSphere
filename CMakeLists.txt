cmake_minimum_required(VERSION 3.24)

set(IDF_TARGET "esp32s3")
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PRINTSPHERE_RELEASE_VERSION LIMIT_COUNT 1)
set(PROJECT_VER "${PRINTSPHERE_RELEASE_VERSION}")
set(PRINTSPHERE_HW_VARIANT "lcd_2_8c" CACHE STRING
    "PrintSphere hardware variant: lcd_2_8c or amoled_1_75")
set_property(CACHE PRINTSPHERE_HW_VARIANT PROPERTY STRINGS "lcd_2_8c" "amoled_1_75")

if(PRINTSPHERE_HW_VARIANT STREQUAL "lcd_2_8c")
    set(PRINTSPHERE_HW_VARIANT_LABEL "2.8C")
    set(PRINTSPHERE_BSP_COMPONENT "esp32_s3_touch_lcd_2_8c")
    set(PRINTSPHERE_HW_COMPILE_DEFINE "PRINTSPHERE_HW_VARIANT_LCD_2_8C=1")
    set(PRINTSPHERE_RELEASE_ROOT "${CMAKE_SOURCE_DIR}/release/2.8c")
    set(PRINTSPHERE_RELEASE_VERSION_SUFFIX "${PRINTSPHERE_RELEASE_VERSION}-2.8c")
    set(EXCLUDE_COMPONENTS esp32_s3_touch_amoled_1_75)
elseif(PRINTSPHERE_HW_VARIANT STREQUAL "amoled_1_75")
    set(PRINTSPHERE_HW_VARIANT_LABEL "1.75 AMOLED")
    set(PRINTSPHERE_BSP_COMPONENT "esp32_s3_touch_amoled_1_75")
    set(PRINTSPHERE_HW_COMPILE_DEFINE "PRINTSPHERE_HW_VARIANT_AMOLED_1_75=1")
    set(PRINTSPHERE_RELEASE_ROOT "${CMAKE_SOURCE_DIR}/release")
    set(PRINTSPHERE_RELEASE_VERSION_SUFFIX "${PRINTSPHERE_RELEASE_VERSION}")
    set(EXCLUDE_COMPONENTS esp32_s3_touch_lcd_2_8c)
else()
    message(FATAL_ERROR
        "Unsupported PRINTSPHERE_HW_VARIANT='${PRINTSPHERE_HW_VARIANT}'. "
        "Use 'lcd_2_8c' or 'amoled_1_75'.")
endif()

if(NOT DEFINED SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig" CACHE FILEPATH "Board-specific sdkconfig")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(PRINTSPHERE_HW_VARIANT STREQUAL "lcd_2_8c")
    set(PRINTSPHERE_PROJECT_ID "printsphere_2_8c_p2")
else()
    set(PRINTSPHERE_PROJECT_ID "printsphere_1_75_p2")
endif()
project(${PRINTSPHERE_PROJECT_ID})
if(CONFIG_LV_USE_FREERTOS_TASK_NOTIFY OR CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES LESS 2)
    message(FATAL_ERROR
        "Unsafe display synchronization in cached sdkconfig: disable "
        "CONFIG_LV_USE_FREERTOS_TASK_NOTIFY and set "
        "CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES=2.")
endif()
if(CONFIG_MBEDTLS_DEBUG AND NOT PRINTSPHERE_RELEASE_VERSION MATCHES "[-_]debug")
    message(FATAL_ERROR
        "TLS tracing is enabled in the cached sdkconfig. Disable CONFIG_MBEDTLS_DEBUG "
        "(Component config > mbedTLS > Enable mbedTLS debugging). "
        "Tracing is only allowed for an explicitly named -debug/_debug version.")
endif()
if(NOT "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}" STREQUAL "5.5.4")
    message(FATAL_ERROR "PrintSphere requires tested ESP-IDF v5.5.4")
endif()

# Managed components are available after project(). Fail the build if the
# required display safety patch cannot be applied to the pinned dependency.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/patches/apply_adapter_patches.py"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" RESULT_VARIABLE PRINTSPHERE_PATCH_RESULT)
if(NOT PRINTSPHERE_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Required LVGL adapter safety patch failed")
endif()

# Work around an Xtensa GCC ICE seen in ESP-IDF 5.5.2 while compiling
# esp_lcd_panel_rgb.c with the default debug/-Og flags on Windows.
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

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(RELEASE_INITIAL_FLASH_DIR "${PRINTSPHERE_RELEASE_ROOT}/initial")
set(RELEASE_INITIAL_FLASH_IMAGE "${RELEASE_INITIAL_FLASH_DIR}/printsphere_full.bin")
set(RELEASE_INITIAL_FLASH_VERSIONED_IMAGE
    "${RELEASE_INITIAL_FLASH_DIR}/archive/printsphere_full-${PRINTSPHERE_RELEASE_VERSION_SUFFIX}.bin")
set(RELEASE_OTA_DIR "${PRINTSPHERE_RELEASE_ROOT}/ota")
set(RELEASE_OTA_IMAGE "${RELEASE_OTA_DIR}/printsphere_ota.bin")
set(RELEASE_OTA_VERSIONED_IMAGE
    "${RELEASE_OTA_DIR}/archive/printsphere_ota-${PRINTSPHERE_RELEASE_VERSION_SUFFIX}.bin")
set(RELEASE_INITIAL_FLASH_PACKAGER "${CMAKE_SOURCE_DIR}/tools/package_initial_flash.py")
# Error lookup TSV is embedded directly into the app binary via EMBED_TXTFILES
# in main/CMakeLists.txt — no SPIFFS partition needed.

add_custom_target(
    release_initial_flash
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RELEASE_INITIAL_FLASH_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RELEASE_OTA_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${RELEASE_INITIAL_FLASH_PACKAGER}"
            --build-dir "${CMAKE_BINARY_DIR}"
            --release-root "${PRINTSPHERE_RELEASE_ROOT}"
            --output "${RELEASE_INITIAL_FLASH_IMAGE}"
            --ota-output "${RELEASE_OTA_IMAGE}"
            --version "${PRINTSPHERE_RELEASE_VERSION_SUFFIX}"
    BYPRODUCTS
            "${RELEASE_INITIAL_FLASH_IMAGE}"
            "${RELEASE_INITIAL_FLASH_VERSIONED_IMAGE}"
            "${RELEASE_OTA_IMAGE}"
            "${RELEASE_OTA_VERSIONED_IMAGE}"
    DEPENDS
            app
            bootloader
            "${CMAKE_BINARY_DIR}/flasher_args.json"
            "${RELEASE_INITIAL_FLASH_PACKAGER}"
    COMMENT "Packaging ${PRINTSPHERE_HW_VARIANT_LABEL} release images"
    VERBATIM
)
