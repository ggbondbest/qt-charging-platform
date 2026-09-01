add_library(charging_database_resources INTERFACE)
add_library(ChargingPlatform::DatabaseResources ALIAS charging_database_resources)

# Propagating the .qrc as an INTERFACE source compiles the SQL into each final
# consumer executable, avoiding runtime dependence on the working directory.
target_sources(charging_database_resources
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/database.qrc
)
