add_library(charging_client_station_pages STATIC
    login_page.cpp
    login_page.h
)
add_library(ChargingPlatform::ClientStationPages ALIAS charging_client_station_pages)

target_include_directories(charging_client_station_pages PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(charging_client_station_pages
    PUBLIC
    ChargingPlatform::ClientStationServices
    ChargingPlatform::ClientWidgets
    ChargingPlatform::Common
    Qt6::Widgets
    PRIVATE
    ChargingPlatform::ProjectOptions
)
