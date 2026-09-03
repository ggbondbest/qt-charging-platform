add_library(charging_server_repositories STATIC
    charging_repository.cpp
    charging_repository.h
    order_repository.cpp
    order_repository.h
    repository_row_mapper.h
    user_repository.cpp
    user_repository.h
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
