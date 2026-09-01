add_subdirectory(database)
add_subdirectory(repositories)
add_subdirectory(services)
add_subdirectory(network)
add_subdirectory(pages)

add_executable(charging_server
    app/main.cpp
)
target_link_libraries(charging_server PRIVATE
    ChargingPlatform::DatabaseResources
    ChargingPlatform::ServerPages
    ChargingPlatform::ServerCore
    ChargingPlatform::ServerServices
    ChargingPlatform::ProjectOptions
    Qt6::Sql
    Qt6::Widgets
)
target_compile_definitions(charging_server PRIVATE
    CHARGING_PLATFORM_VERSION="${PROJECT_VERSION}"
)
set_target_properties(charging_server PROPERTIES
    OUTPUT_NAME "charging-server"
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

install(TARGETS charging_server
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
