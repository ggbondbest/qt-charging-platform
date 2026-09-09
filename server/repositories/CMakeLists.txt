add_library(charging_server_repositories STATIC
    queue_repository.cpp queue_repository.h
    repair_repository.cpp repair_repository.h
    charging_target_repository.cpp charging_target_repository.h
    admin_charger_extensions.cpp
    admin_charger_extensions.h
    admin_order_billing.cpp
    admin_order_billing.h
    admin_repository.cpp
    admin_repository.h
    charger_repository.cpp
    charger_repository.h
    dashboard_repository.cpp
    dashboard_repository.h
    charging_repository.cpp
    charging_repository.h
    order_repository.cpp
    order_repository.h
    operation_log_repository.cpp
    operation_log_repository.h
    recharge_repository.cpp
    recharge_repository.h
    repository_row_mapper.h
    reservation_query_repository.cpp
    reservation_query_repository.h
    station_repository.cpp
    station_repository.h
    user_repository.cpp
    user_repository.h
    user_api_repository.cpp
    user_api_repository.h
    workflow_repository_types.h
)
add_library(ChargingPlatform::ServerRepositories ALIAS charging_server_repositories)

target_include_directories(charging_server_repositories PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(charging_server_repositories
    PUBLIC
        ChargingPlatform::ServerDatabase
        ChargingPlatform::Common
        ChargingPlatform::ProjectOptions
        Qt6::Core
        Qt6::Sql
)
