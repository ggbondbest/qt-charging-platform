add_library(charging_client_settings_services STATIC
    settings_service.cpp
    settings_service.h
)
add_library(ChargingPlatform::ClientSettingsServices ALIAS charging_client_settings_services)

target_include_directories(charging_client_settings_services PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(charging_client_settings_services
    PUBLIC
    ChargingPlatform::Common
    Qt6::Core
    PRIVATE
    ChargingPlatform::ProjectOptions
)
