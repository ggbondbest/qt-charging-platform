add_library(charging_server_core STATIC
    charging_server.cpp
    charging_server.h
)
add_library(ChargingPlatform::ServerCore ALIAS charging_server_core)
add_library(ChargingPlatform::ServerNetwork ALIAS charging_server_core)

target_include_directories(charging_server_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(charging_server_core
    PUBLIC
        ChargingPlatform::Common
        ChargingPlatform::ProjectOptions
        Qt6::Core
        Qt6::Network
)
