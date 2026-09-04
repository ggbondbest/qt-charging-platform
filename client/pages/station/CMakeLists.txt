# Qt WebEngine 为可选模块（腾讯地图面板）。未安装时 StationMapPanel 自动降级为
# 提示 + 电站列表，不影响编译与主流程；见 qt_6_2_4_compatibility.md 第 7 节。
find_package(Qt6 QUIET COMPONENTS WebEngineWidgets)

add_library(charging_client_station_pages STATIC
    home_shell.cpp
    home_shell.h
    login_page.cpp
    login_page.h
    navigation_page.cpp
    navigation_page.h
    platform_theme.cpp
    platform_theme.h
    reservation_completed_page.cpp
    reservation_completed_page.h
    reservation_confirm_page.cpp
    reservation_confirm_page.h
    reservation_module_page.cpp
    reservation_module_page.h
    reservation_order_page.cpp
    reservation_order_page.h
    settings_page.cpp
    settings_page.h
    station_detail_page.cpp
    station_detail_page.h
    station_home_page.cpp
    station_home_page.h
    station_map_panel.cpp
    station_map_panel.h
    station_resources.qrc
    map/tencent_map.html
    ${CMAKE_SOURCE_DIR}/resources/client_platform.qrc
)
add_library(ChargingPlatform::ClientStationPages ALIAS charging_client_station_pages)

target_include_directories(charging_client_station_pages PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(charging_client_station_pages
    PUBLIC
    ChargingPlatform::ClientStationServices
    ChargingPlatform::ClientReservationServices
    ChargingPlatform::ClientSettingsServices
    ChargingPlatform::ClientProfileChargingPages
    ChargingPlatform::ClientProfileChargingServices
    ChargingPlatform::ClientWidgets
    ChargingPlatform::Common
    Qt6::Widgets
    PRIVATE
    ChargingPlatform::ProjectOptions
)

if(Qt6WebEngineWidgets_FOUND)
    target_link_libraries(charging_client_station_pages PRIVATE Qt6::WebEngineWidgets)
    target_compile_definitions(charging_client_station_pages PRIVATE
        CHARGING_PLATFORM_HAS_WEBENGINE=1
    )
endif()
