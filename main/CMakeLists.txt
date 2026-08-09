set(PRINTSPHERE_MAIN_REQUIRES
    app_update
    esp_driver_uart
    esp_driver_usb_serial_jtag
    esp_pm
    esp_https_ota
    esp_driver_gpio
    esp_event
    esp_http_client
    esp_http_server
    mqtt
    esp_netif
    lwip
    espressif__esp_new_jpeg
    espressif__esp_codec_dev
    esp-tls
    esp_wifi
    libpng
    lvgl
    nvs_flash
    joltwallet__littlefs
)

set(PRINTSPHERE_MAIN_PRIV_REQUIRES)

if("${PRINTSPHERE_HW_COMPILE_DEFINE}" STREQUAL "PRINTSPHERE_HW_VARIANT_AMOLED_1_75=1")
    list(APPEND PRINTSPHERE_MAIN_PRIV_REQUIRES
        XPowersLib
        esp32_s3_touch_amoled_1_75
    )
elseif("${PRINTSPHERE_HW_COMPILE_DEFINE}" STREQUAL "PRINTSPHERE_HW_VARIANT_LCD_2_8C=1")
    list(APPEND PRINTSPHERE_MAIN_PRIV_REQUIRES
        esp_adc
        esp32_s3_touch_lcd_2_8c
    )
elseif("${PRINTSPHERE_HW_COMPILE_DEFINE}" STREQUAL "PRINTSPHERE_HW_VARIANT_KNOMI_V2=1")
    list(APPEND PRINTSPHERE_MAIN_PRIV_REQUIRES
        btt_knomi_v2
    )
endif()

idf_component_register(
    SRCS
        "app_main.cpp"
        "src/application.cpp"
        "src/audio_notifier.cpp"
        "src/bambu_status.cpp"
        "src/bambu_cloud_client.cpp"
        "src/config_store.cpp"
        "src/error_lookup.cpp"
        "include/font/dosis_20.c"
        "include/font/dosis_32.c"
        "include/font/dosis_40.c"
        "include/img/bambuicon.c"
        "include/font/mdi_30.c"
        "include/font/mdi_40.c"
        "src/pmu.cpp"
        "src/p1s_camera_client.cpp"
        "src/printer_client.cpp"
        "src/printer_state.cpp"
        "src/debug_log_buffer.cpp"
        "src/setup_portal.cpp"
        "src/serial_provisioner.cpp"
        "src/status_resolver.cpp"
        "src/time_sync.cpp"
        "src/ui.cpp"
        "src/wifi_manager.cpp"
    INCLUDE_DIRS
        "include"
    EMBED_TXTFILES
        "include/certs/bambu.cert"
        "include/certs/bambu_p2s_250626.cert"
        "include/certs/bambu_h2c_251122.cert"
        "include/certs/bambu_x2c_260425.cert"
        "include/error_lookup/error_lookup.tsv"
    REQUIRES
        ${PRINTSPHERE_MAIN_REQUIRES}
    PRIV_REQUIRES
        ${PRINTSPHERE_MAIN_PRIV_REQUIRES}
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PRINTSPHERE_RELEASE_VERSION="${PRINTSPHERE_RELEASE_VERSION}"
    ${PRINTSPHERE_HW_COMPILE_DEFINE}
)

if("${PRINTSPHERE_HW_COMPILE_DEFINE}" STREQUAL "PRINTSPHERE_HW_VARIANT_AMOLED_1_75=1")
    idf_component_get_property(PRINTSPHERE_BSP_DIR esp32_s3_touch_amoled_1_75 COMPONENT_DIR)
    idf_component_get_property(PRINTSPHERE_BSP_LIB esp32_s3_touch_amoled_1_75 COMPONENT_LIB)
    idf_component_get_property(PRINTSPHERE_XPOWERS_DIR XPowersLib COMPONENT_DIR)
    idf_component_get_property(PRINTSPHERE_XPOWERS_LIB XPowersLib COMPONENT_LIB)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${PRINTSPHERE_BSP_DIR}/include"
        "${PRINTSPHERE_XPOWERS_DIR}/src"
    )
    target_link_libraries(${COMPONENT_LIB} PRIVATE
        ${PRINTSPHERE_BSP_LIB}
        ${PRINTSPHERE_XPOWERS_LIB}
    )
elseif("${PRINTSPHERE_HW_COMPILE_DEFINE}" STREQUAL "PRINTSPHERE_HW_VARIANT_LCD_2_8C=1")
    idf_component_get_property(PRINTSPHERE_BSP_DIR esp32_s3_touch_lcd_2_8c COMPONENT_DIR)
    idf_component_get_property(PRINTSPHERE_BSP_LIB esp32_s3_touch_lcd_2_8c COMPONENT_LIB)
    idf_component_get_property(PRINTSPHERE_ADC_DIR esp_adc COMPONENT_DIR)
    idf_component_get_property(PRINTSPHERE_ADC_LIB esp_adc COMPONENT_LIB)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${PRINTSPHERE_BSP_DIR}/include"
        "${PRINTSPHERE_ADC_DIR}/include"
    )
    target_link_libraries(${COMPONENT_LIB} PRIVATE
        ${PRINTSPHERE_BSP_LIB}
        ${PRINTSPHERE_ADC_LIB}
    )
elseif("${PRINTSPHERE_HW_COMPILE_DEFINE}" STREQUAL "PRINTSPHERE_HW_VARIANT_KNOMI_V2=1")
    idf_component_get_property(PRINTSPHERE_BSP_DIR btt_knomi_v2 COMPONENT_DIR)
    idf_component_get_property(PRINTSPHERE_BSP_LIB btt_knomi_v2 COMPONENT_LIB)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${PRINTSPHERE_BSP_DIR}/include"
    )
    target_link_libraries(${COMPONENT_LIB} PRIVATE
        ${PRINTSPHERE_BSP_LIB}
    )
endif()

string(FIND "${PRINTSPHERE_RELEASE_VERSION}" "-debug" _debug_pos)
string(FIND "${PRINTSPHERE_RELEASE_VERSION}" "_debug" _debug_pos2)
if(_debug_pos GREATER -1 OR _debug_pos2 GREATER -1)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PRINTSPHERE_DEBUG_BUILD=1)
endif()
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
