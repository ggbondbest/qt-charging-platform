add_library(charging_client_favorites_services STATIC
    favorites_service.cpp
    favorites_service.h
    notification_service.cpp
    notification_service.h
)
add_library(ChargingPlatform::ClientFavoritesServices ALIAS charging_client_favorites_services)

target_include_directories(charging_client_favorites_services PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(charging_client_favorites_services
    PUBLIC
    ChargingPlatform::Common
    ChargingPlatform::ClientSettingsServices
    Qt6::Core
    PRIVATE
    ChargingPlatform::ProjectOptions
)
