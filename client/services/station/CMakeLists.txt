add_library(charging_client_station_services INTERFACE)
add_library(ChargingPlatform::ClientStationServices ALIAS charging_client_station_services)

target_link_libraries(charging_client_station_services INTERFACE
    ChargingPlatform::ClientNetwork
    ChargingPlatform::Common
    ChargingPlatform::ProjectOptions
    Qt6::Core
)
