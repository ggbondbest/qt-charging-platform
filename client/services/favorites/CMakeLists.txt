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
    # 2026-09-08 通知服务端通道（横闯，成员 3）：IRequestTransport 头在
    # profile_charging 域；该向依赖单向（profile 不链 favorites），无环。
    ChargingPlatform::ClientProfileChargingServices
    Qt6::Core
    PRIVATE
    ChargingPlatform::ProjectOptions
)
