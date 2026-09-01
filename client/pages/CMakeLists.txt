add_subdirectory(station)
add_subdirectory(profile_charging)

add_library(charging_client_pages INTERFACE)
add_library(ChargingPlatform::ClientPages ALIAS charging_client_pages)

target_link_libraries(charging_client_pages INTERFACE
    ChargingPlatform::ClientStationPages
    ChargingPlatform::ClientProfileChargingPages
)
