add_library(charging_client_map_services STATIC
    map_geo_service.cpp
    map_geo_service.h
)
add_library(ChargingPlatform::ClientMapServices ALIAS charging_client_map_services)

target_include_directories(charging_client_map_services PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(charging_client_map_services
    PUBLIC
    Qt6::Core
    Qt6::Network
    PRIVATE
    ChargingPlatform::ProjectOptions
)
