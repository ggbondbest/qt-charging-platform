add_library(charging_server_pages STATIC
    main_window.cpp
    main_window.h
)
add_library(ChargingPlatform::ServerPages ALIAS charging_server_pages)

target_include_directories(charging_server_pages PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(charging_server_pages
    PUBLIC
        ChargingPlatform::ServerCore
        ChargingPlatform::ProjectOptions
        Qt6::Widgets
)
