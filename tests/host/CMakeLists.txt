cmake_minimum_required(VERSION 3.16)
project(printsphere_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(FetchContent)
set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE STRING "" FORCE)
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.2
)
FetchContent_MakeAvailable(doctest)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_executable(host_tests
    main_test.cpp
    test_bambu_status.cpp
    test_printer_state.cpp
    test_status_resolver.cpp
    stubs/error_lookup_stub.cpp
    "${REPO_ROOT}/main/src/bambu_status.cpp"
    "${REPO_ROOT}/main/src/printer_state.cpp"
    "${REPO_ROOT}/main/src/status_resolver.cpp"
)

target_include_directories(host_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${REPO_ROOT}/main/include"
)

target_link_libraries(host_tests PRIVATE doctest::doctest)

enable_testing()
include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
doctest_discover_tests(host_tests)
