add_library(charging_server_repositories INTERFACE)
add_library(ChargingPlatform::ServerRepositories ALIAS charging_server_repositories)

target_link_libraries(charging_server_repositories INTERFACE
    ChargingPlatform::ServerDatabase
    ChargingPlatform::Common
    ChargingPlatform::ProjectOptions
    Qt6::Core
    Qt6::Sql
)
