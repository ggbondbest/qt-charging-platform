add_library(charging_dashboard_service STATIC
    dashboard_service_manager.cpp
    dashboard_service_manager.h
)
add_library(ChargingPlatform::DashboardService ALIAS charging_dashboard_service)
target_include_directories(charging_dashboard_service PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(charging_dashboard_service PUBLIC Qt6::Core ChargingPlatform::ProjectOptions)
target_compile_definitions(charging_dashboard_service PRIVATE
    CHARGING_DASHBOARD_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
)
