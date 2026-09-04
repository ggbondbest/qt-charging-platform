add_library(charging_client_station_pages INTERFACE)
add_library(ChargingPlatform::ClientStationPages ALIAS charging_client_station_pages)

target_link_libraries(charging_client_station_pages INTERFACE
    ChargingPlatform::ClientStationServices
    ChargingPlatform::ClientWidgets
    ChargingPlatform::Common
    ChargingPlatform::ProjectOptions
    Qt6::Widgets
)
