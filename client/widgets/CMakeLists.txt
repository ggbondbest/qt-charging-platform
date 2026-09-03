add_library(charging_client_widgets STATIC
    include/charging/client/widgets/action_button.h
    include/charging/client/widgets/card.h
    include/charging/client/widgets/clickable_card.h
    include/charging/client/widgets/loading_overlay.h
    include/charging/client/widgets/notice_panel.h
    include/charging/client/widgets/status_tag.h
    include/charging/client/widgets/toast.h
    src/action_button.cpp
    src/card.cpp
    src/clickable_card.cpp
    src/loading_overlay.cpp
    src/notice_panel.cpp
    src/status_tag.cpp
    src/toast.cpp
)
add_library(ChargingPlatform::ClientWidgets ALIAS charging_client_widgets)

target_include_directories(charging_client_widgets PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(charging_client_widgets PUBLIC
    ChargingPlatform::ProjectOptions
    Qt6::Widgets
)
