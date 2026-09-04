add_library(charging_client_network INTERFACE)
add_library(ChargingPlatform::ClientNetwork ALIAS charging_client_network)

target_link_libraries(charging_client_network INTERFACE
    ChargingPlatform::Common
    ChargingPlatform::ProjectOptions
    Qt6::Core
    Qt6::Network
)
