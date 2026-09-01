cmake_minimum_required(VERSION 3.16)

project(
    ChargingPlatform
    VERSION 0.1.0
    DESCRIPTION "Electric vehicle charging platform"
    LANGUAGES CXX
)

include(CTest)

set(CHARGING_PLATFORM_QT_BASELINE_VERSION "6.2.4")
option(CHARGING_PLATFORM_STRICT_QT_VERSION
    "Reject Qt versions other than the configured baseline (used by CI)" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(_charging_qt_components Core Gui Network Sql Widgets)
if(BUILD_TESTING)
    list(APPEND _charging_qt_components Test)
endif()

# A minimum version keeps newer local Qt installations usable. The CI build turns
# CHARGING_PLATFORM_STRICT_QT_VERSION on and therefore compiles with exactly 6.2.4.
find_package(Qt6 ${CHARGING_PLATFORM_QT_BASELINE_VERSION} REQUIRED
    COMPONENTS ${_charging_qt_components})

if(CHARGING_PLATFORM_STRICT_QT_VERSION
   AND NOT "${Qt6_VERSION}" VERSION_EQUAL "${CHARGING_PLATFORM_QT_BASELINE_VERSION}")
    message(FATAL_ERROR
        "Strict Qt check requires ${CHARGING_PLATFORM_QT_BASELINE_VERSION}, "
        "but CMake found ${Qt6_VERSION}")
endif()

message(STATUS
    "ChargingPlatform: Qt ${Qt6_VERSION} "
    "(compatibility baseline ${CHARGING_PLATFORM_QT_BASELINE_VERSION})")

add_library(charging_project_options INTERFACE)
add_library(ChargingPlatform::ProjectOptions ALIAS charging_project_options)
target_compile_features(charging_project_options INTERFACE cxx_std_17)
target_compile_options(charging_project_options INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wextra>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<CXX_COMPILER_ID:MSVC>:/permissive->
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>
)

add_subdirectory(common)
add_subdirectory(database)
add_subdirectory(client)
add_subdirectory(server)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

unset(_charging_qt_components)
