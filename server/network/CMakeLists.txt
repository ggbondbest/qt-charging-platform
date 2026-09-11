add_library(charging_server_core STATIC
    admin_request_gateway.cpp
    admin_request_gateway.h
    client_session.cpp
    client_session.h
    charging_server.cpp
    charging_server.h
    request_dispatcher.cpp
    request_dispatcher.h
    server_runtime.cpp
    server_runtime.h
    tcp_server.h
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
    PRIVATE
        ChargingPlatform::ServerServices
)
