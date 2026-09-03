add_library(charging_client_network STATIC
    client_connection.cpp
    client_connection.h
)
add_library(ChargingPlatform::ClientNetwork ALIAS charging_client_network)

target_include_directories(charging_client_network PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(charging_client_network
    PUBLIC
    ChargingPlatform::Common
    Qt6::Core
    Qt6::Network
    PRIVATE
    ChargingPlatform::ProjectOptions
)
