add_library(charging_client_profile_charging_pages INTERFACE)
add_library(ChargingPlatform::ClientProfileChargingPages ALIAS
    charging_client_profile_charging_pages)

target_link_libraries(charging_client_profile_charging_pages INTERFACE
    ChargingPlatform::ClientProfileChargingServices
    ChargingPlatform::ClientWidgets
    ChargingPlatform::Common
    ChargingPlatform::ProjectOptions
    Qt6::Widgets
)
