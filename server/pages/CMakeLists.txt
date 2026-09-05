add_library(charging_server_pages STATIC
    admin_mock_data.cpp
    admin_mock_data.h
    activity_records_page.cpp
    activity_records_page.h
    admin_login_page.cpp
    admin_login_page.h
    charger_management_page.cpp
    charger_management_page.h
    dashboard_page.cpp
    dashboard_page.h
    dashboard_visual_widgets.cpp
    dashboard_visual_widgets.h
    main_window.cpp
    main_window.h
    management_page_widgets.cpp
    management_page_widgets.h
    order_management_page.cpp
    order_management_page.h
    station_management_page.cpp
    station_management_page.h
    user_management_page.cpp
    user_management_page.h
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
