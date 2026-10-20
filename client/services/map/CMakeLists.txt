add_library(charging_client_map_services STATIC
    map_geo_service.cpp
    map_geo_service.h
    map_service_config.cpp
    map_service_config.h
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

# Only a developer path is compiled in; credentials are read at runtime.
target_compile_definitions(charging_client_map_services PRIVATE
    CHARGING_MAP_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)
