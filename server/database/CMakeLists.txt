add_library(charging_server_database STATIC
    database_connection.cpp
    database_connection.h
)
add_library(ChargingPlatform::ServerDatabase ALIAS charging_server_database)

target_include_directories(charging_server_database PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(charging_server_database
    PUBLIC
        ChargingPlatform::ProjectOptions
        Qt6::Core
        Qt6::Sql
)
