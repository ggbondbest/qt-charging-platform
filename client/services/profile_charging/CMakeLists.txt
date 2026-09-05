add_library(charging_client_profile_charging_services STATIC
    include/charging/client/profile_charging/charging_service.h
    include/charging/client/profile_charging/client_errors.h
    include/charging/client/profile_charging/i_request_transport.h
    include/charging/client/profile_charging/mock_request_transport.h
    include/charging/client/profile_charging/order_service.h
    include/charging/client/profile_charging/presentation_format.h
    include/charging/client/profile_charging/wallet_service.h
    src/charging_service.cpp
    src/client_errors.cpp
    src/mock_request_transport.cpp
    src/network_request_transport.cpp
    include/charging/client/profile_charging/network_request_transport.h
    src/order_service.cpp
    src/presentation_format.cpp
    src/wallet_service.cpp
)
add_library(ChargingPlatform::ClientProfileChargingServices ALIAS
    charging_client_profile_charging_services)

target_include_directories(charging_client_profile_charging_services PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(charging_client_profile_charging_services PUBLIC
    ChargingPlatform::ClientNetwork
    ChargingPlatform::Common
    ChargingPlatform::ProjectOptions
    Qt6::Core
)
