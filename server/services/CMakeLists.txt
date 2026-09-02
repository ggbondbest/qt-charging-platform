add_library(charging_server_services STATIC
    user_service.cpp
    user_service.h
)
add_library(ChargingPlatform::ServerServices ALIAS charging_server_services)

target_include_directories(charging_server_services PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(charging_server_services
    PUBLIC
        ChargingPlatform::ServerRepositories
        ChargingPlatform::Common
        ChargingPlatform::ProjectOptions
        Qt6::Core
)
