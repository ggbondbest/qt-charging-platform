add_library(charging_client_profile_charging_services INTERFACE)
add_library(ChargingPlatform::ClientProfileChargingServices ALIAS
    charging_client_profile_charging_services)

target_link_libraries(charging_client_profile_charging_services INTERFACE
    ChargingPlatform::ClientNetwork
    ChargingPlatform::Common
    ChargingPlatform::ProjectOptions
    Qt6::Core
)
