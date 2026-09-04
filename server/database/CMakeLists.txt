add_library(charging_server_database INTERFACE)
add_library(ChargingPlatform::ServerDatabase ALIAS charging_server_database)

target_link_libraries(charging_server_database INTERFACE
    ChargingPlatform::ProjectOptions
    Qt6::Core
    Qt6::Sql
)
