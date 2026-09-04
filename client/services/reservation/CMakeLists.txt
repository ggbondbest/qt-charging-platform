add_library(charging_client_reservation_services STATIC
    reservation_service.cpp
    reservation_service.h
)
add_library(ChargingPlatform::ClientReservationServices ALIAS charging_client_reservation_services)

target_include_directories(charging_client_reservation_services PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(charging_client_reservation_services
    PUBLIC
    ChargingPlatform::ClientNetwork
    ChargingPlatform::ClientSettingsServices
    ChargingPlatform::Common
    Qt6::Core
    PRIVATE
    ChargingPlatform::ProjectOptions
)
