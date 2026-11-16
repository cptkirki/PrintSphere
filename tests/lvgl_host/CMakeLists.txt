cmake_minimum_required(VERSION 3.24)
project(printsphere_lvgl_host C CXX)
find_package(Threads REQUIRED)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${REPO}/managed_components/lvgl__lvgl" lvgl)
foreach(board IN ITEMS amoled lcd)
  add_executable(ui-${board}-tests ui_tests.cpp mock_board.cpp
      "${REPO}/main/src/ui.cpp" "${REPO}/main/src/printer_state.cpp"
      "${REPO}/main/include/font/dosis_20.c" "${REPO}/main/include/font/dosis_32.c"
      "${REPO}/main/include/font/dosis_40.c"
      "${REPO}/main/include/font/montserrat_12.c" "${REPO}/main/include/font/montserrat_14.c" "${REPO}/main/include/font/montserrat_20.c" "${REPO}/main/include/font/mdi_30.c"
      "${REPO}/main/include/font/mdi_40.c" "${REPO}/main/include/img/bambuicon.c")
  target_include_directories(ui-${board}-tests PRIVATE stubs "${REPO}/tests/stubs" "${REPO}/main/include" "${REPO}/managed_components/lvgl__lvgl/src")
  if(board STREQUAL "amoled")
    target_compile_definitions(ui-${board}-tests PRIVATE PRINTSPHERE_HW_VARIANT_AMOLED_1_75=1)
  else()
    target_compile_definitions(ui-${board}-tests PRIVATE PRINTSPHERE_HW_VARIANT_LCD_2_8C=1)
  endif()
  target_link_libraries(ui-${board}-tests PRIVATE lvgl Threads::Threads)
endforeach()
