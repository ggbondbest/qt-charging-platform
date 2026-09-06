add_library(charging_server_services STATIC
    admin_service.cpp
    admin_service.h
    billing_service.cpp
    billing_service.h
    charging_service.cpp
    charging_service.h
    charging_state_machine.cpp
    charging_state_machine.h
    order_service.cpp
    order_service.h
    service_clock.h
    user_service.cpp
    user_service.h
    user_api_service.cpp
    user_api_service.h
)
add_library(ChargingPlatform::ServerServices ALIAS charging_server_services)

target_include_directories(charging_server_services PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(charging_server_services
    PUBLIC
        ChargingPlatform::ServerRepositories
        ChargingPlatform::Common
        ChargingPlatform::ProjectOptions
        Qt6::Core
)
