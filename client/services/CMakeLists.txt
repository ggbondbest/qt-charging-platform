add_subdirectory(station)
add_subdirectory(reservation)
add_subdirectory(profile_charging)
add_subdirectory(settings)
add_subdirectory(map)

add_library(charging_client_services INTERFACE)
add_library(ChargingPlatform::ClientServices ALIAS charging_client_services)

target_link_libraries(charging_client_services INTERFACE
    ChargingPlatform::ClientStationServices
    ChargingPlatform::ClientReservationServices
    ChargingPlatform::ClientProfileChargingServices
    ChargingPlatform::ClientSettingsServices
    ChargingPlatform::ClientMapServices
)
