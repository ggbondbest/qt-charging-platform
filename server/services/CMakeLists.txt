add_library(charging_server_services INTERFACE)
add_library(ChargingPlatform::ServerServices ALIAS charging_server_services)

target_link_libraries(charging_server_services INTERFACE
    ChargingPlatform::ServerRepositories
    ChargingPlatform::Common
    ChargingPlatform::ProjectOptions
    Qt6::Core
)
