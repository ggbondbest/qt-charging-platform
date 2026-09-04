add_library(charging_client_widgets INTERFACE)
add_library(ChargingPlatform::ClientWidgets ALIAS charging_client_widgets)

target_link_libraries(charging_client_widgets INTERFACE
    ChargingPlatform::ProjectOptions
    Qt6::Widgets
)
