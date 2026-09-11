add_library(charging_client_profile_charging_pages STATIC
    include/charging/client/profile_charging/avatar_library.h
    include/charging/client/profile_charging/charging_page.h
    include/charging/client/profile_charging/charging_home_page.h
    include/charging/client/profile_charging/charging_pulse.h
    include/charging/client/profile_charging/order_detail_page.h
    include/charging/client/profile_charging/order_list_page.h
    include/charging/client/profile_charging/order_status_display.h
    include/charging/client/profile_charging/profile_edit_page.h
    include/charging/client/profile_charging/profile_page.h
    include/charging/client/profile_charging/recharge_page.h
    include/charging/client/profile_charging/settlement_page.h
    include/charging/client/profile_charging/wallet_page.h
    src/avatar_library.cpp
    src/charging_page.cpp
    src/charging_home_page.cpp
    src/charging_pulse.cpp
    src/order_detail_page.cpp
    src/order_list_page.cpp
    src/order_status_display.cpp
    src/profile_edit_page.cpp
    src/profile_page.cpp
    src/recharge_page.cpp
    src/settlement_page.cpp
    src/wallet_page.cpp
)
add_library(ChargingPlatform::ClientProfileChargingPages ALIAS
    charging_client_profile_charging_pages)

target_include_directories(charging_client_profile_charging_pages PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(charging_client_profile_charging_pages PUBLIC
    ChargingPlatform::ClientProfileChargingServices
    ChargingPlatform::ClientReservationServices
    ChargingPlatform::ClientWidgets
    ChargingPlatform::Common
    ChargingPlatform::ProjectOptions
    Qt6::Widgets
)

# Local visual-development shell only (mock transport, no real network).
# Not part of charging_client; remove or fold into the app assembly once the
# leader owns client navigation.
add_executable(charging_profile_preview
    preview/preview_main.cpp
    ${CMAKE_SOURCE_DIR}/resources/client_platform.qrc
)

target_link_libraries(charging_profile_preview PRIVATE
    ChargingPlatform::ClientProfileChargingPages
    ChargingPlatform::ClientProfileChargingServices
    Qt6::Widgets
)

set_target_properties(charging_profile_preview PROPERTIES
    OUTPUT_NAME "charging-profile-preview"
)

# Local-only screenshot harness for the integrated HomeShell (member 2 chrome
# + member 3 pages) for UI review. Never part of charging_client.
add_executable(charging_shell_preview
    preview/shell_preview_main.cpp
    ${CMAKE_SOURCE_DIR}/resources/client_platform.qrc
)

target_link_libraries(charging_shell_preview PRIVATE
    ChargingPlatform::ClientStationPages
    ChargingPlatform::ClientProfileChargingPages
    Qt6::Widgets
)

set_target_properties(charging_shell_preview PROPERTIES
    OUTPUT_NAME "charging-shell-preview"
)
