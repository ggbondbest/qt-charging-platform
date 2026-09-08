add_subdirectory(network)
add_subdirectory(services)
add_subdirectory(widgets)
add_subdirectory(pages)

# Keep application composition in this stable file. Feature owners add sources only
# to their module-local CMakeLists.txt, so parallel work does not contend here.
if(CHARGING_BUILD_WIDGETS_CLIENT)
add_executable(charging_widgets_client
    app/main.cpp
    app/main_window.cpp
    app/main_window.h
)

target_include_directories(charging_widgets_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/app
)

target_link_libraries(charging_widgets_client PRIVATE
    ChargingPlatform::ClientPages
    ChargingPlatform::ProjectOptions
    Qt6::Widgets
)

target_compile_definitions(charging_widgets_client PRIVATE
    CHARGING_PLATFORM_VERSION="${PROJECT_VERSION}"
)

set_target_properties(charging_widgets_client PROPERTIES
    OUTPUT_NAME "charging-widgets-client"
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

install(TARGETS charging_widgets_client
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
endif()
if(CHARGING_HAS_QML)
    add_subdirectory(qml)
endif()
