add_library(charging_client_widgets STATIC
    include/charging/client/widgets/action_bar.h
    include/charging/client/widgets/action_button.h
    include/charging/client/widgets/bottom_tab_bar.h
    include/charging/client/widgets/card.h
    include/charging/client/widgets/clickable_card.h
    include/charging/client/widgets/loading_overlay.h
    include/charging/client/widgets/notice_panel.h
    include/charging/client/widgets/status_tag.h
    include/charging/client/widgets/toast.h
    include/charging/client/widgets/top_nav_bar.h
    src/action_bar.cpp
    src/action_button.cpp
    src/bottom_tab_bar.cpp
    src/card.cpp
    src/clickable_card.cpp
    src/loading_overlay.cpp
    src/notice_panel.cpp
    src/status_tag.cpp
    src/toast.cpp
    src/top_nav_bar.cpp
)
add_library(ChargingPlatform::ClientWidgets ALIAS charging_client_widgets)

target_include_directories(charging_client_widgets PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(charging_client_widgets PUBLIC
    ChargingPlatform::ProjectOptions
    # TopNavBar 透传登录态需要 model::User。
    ChargingPlatform::Common
    Qt6::Widgets
)
