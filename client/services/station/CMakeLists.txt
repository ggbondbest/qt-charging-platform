add_library(charging_client_station_services STATIC
    auth_service.cpp
    auth_service.h
)
add_library(ChargingPlatform::ClientStationServices ALIAS charging_client_station_services)

target_include_directories(charging_client_station_services PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(charging_client_station_services
    PUBLIC
    ChargingPlatform::ClientNetwork
    ChargingPlatform::Common
    Qt6::Core
    PRIVATE
    ChargingPlatform::ProjectOptions
)
