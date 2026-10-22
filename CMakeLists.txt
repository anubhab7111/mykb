cmake_minimum_required(VERSION 3.28)
project(omen-kbd VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF) # no C++20 modules; skip the per-file dependency scan
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(OMEN_KBD_SANITIZE "Build Debug with AddressSanitizer + UndefinedBehaviorSanitizer" ON)
if(OMEN_KBD_SANITIZE AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)
find_package(sdbus-c++ REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(tomlplusplus REQUIRED)
find_package(CLI11 REQUIRED)
find_package(GTest REQUIRED)

include(cmake/embed_text.cmake)
embed_text(${CMAKE_SOURCE_DIR}/packaging/config.example.toml
           ${CMAKE_BINARY_DIR}/generated/config_example.hpp kConfigExample)

# Library: everything except main.cpp (sources are globbed so tasks only add files).
file(GLOB_RECURSE OMENKBD_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM OMENKBD_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(omenkbd STATIC ${OMENKBD_SOURCES})
target_include_directories(omenkbd PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(omenkbd PUBLIC SDBusCpp::sdbus-c++ nlohmann_json::nlohmann_json
                                     tomlplusplus::tomlplusplus CLI11::CLI11 Threads::Threads)
target_compile_options(omenkbd PUBLIC -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)
target_compile_definitions(omenkbd PUBLIC OMEN_KBD_VERSION="${PROJECT_VERSION}")

# The kernel module's pure core, built for userspace tests (kbuild builds it into the module).
set(HOK_DIR ${CMAKE_SOURCE_DIR}/kernel/hp-omen-kbd)
set(HOK_C_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)
add_library(hokcore STATIC ${HOK_DIR}/core.c)
target_include_directories(hokcore PUBLIC ${HOK_DIR} ${HOK_DIR}/tests)
target_compile_options(hokcore PRIVATE ${HOK_C_WARNINGS})

add_executable(omen-kbd ${CMAKE_SOURCE_DIR}/src/main.cpp)
target_link_libraries(omen-kbd PRIVATE omenkbd)
install(TARGETS omen-kbd RUNTIME DESTINATION bin)

enable_testing()
file(GLOB OMENKBD_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.cpp)
add_executable(omen-kbd-tests ${OMENKBD_TESTS})
target_link_libraries(omen-kbd-tests PRIVATE omenkbd hokcore GTest::gtest_main)
target_compile_definitions(omen-kbd-tests PRIVATE OMEN_KBD_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
include(GoogleTest)
gtest_discover_tests(omen-kbd-tests DISCOVERY_TIMEOUT 30)

add_executable(hok-core-tests ${HOK_DIR}/tests/core_test.c)
target_link_libraries(hok-core-tests PRIVATE hokcore)
target_compile_options(hok-core-tests PRIVATE ${HOK_C_WARNINGS})
add_test(NAME hok-core COMMAND hok-core-tests)
