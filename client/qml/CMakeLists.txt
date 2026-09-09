# Production QML client. Qt Quick and WebEngine are required by the root build.
# Historical Widgets pages remain test fixtures, not the delivered executable.

# Owned component/page lists are accumulated from the subdirectories below.
# Each subdirectory appends its .qml files via PARENT_SCOPE — add your page to
# the CMakeLists.txt inside YOUR directory only.
set(CHARGING_QML_FILES "")
add_subdirectory(platform)
add_subdirectory(pages/profile_charging)
add_subdirectory(pages/station)

add_executable(charging_client
    main.cpp
    app_bridge.cpp
    app_bridge.h
    workflow_bridge.cpp workflow_bridge.h
    service_bridges.cpp
    service_bridges.h
    map_bridge.cpp
    map_bridge.h
    Shell.qml
    pages/PlaceholderPage.qml
    ${CHARGING_QML_FILES}
)
set_target_properties(charging_client PROPERTIES
    OUTPUT_NAME "charging-client"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/client"
)
# Embed the complete tree, including singleton qmldir and JavaScript helpers.
file(GLOB_RECURSE charging_qml_resources CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    "*.qml" "*.js" "qmldir")
qt_add_resources(charging_client "charging_qml_resources"
    PREFIX "/charging" BASE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${charging_qml_resources})
install(TARGETS charging_client RUNTIME DESTINATION bin)
target_link_libraries(charging_client PRIVATE
    ChargingPlatform::ProjectOptions
    ChargingPlatform::Common
    ChargingPlatform::ClientProfileChargingServices
    ChargingPlatform::ClientStationServices
    ChargingPlatform::ClientReservationServices
    ChargingPlatform::ClientSettingsServices
    ChargingPlatform::ClientMapServices
    ChargingPlatform::ClientFavoritesServices
    ChargingPlatform::ClientNetwork
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Widgets
    Qt6::WebEngineQuick
)
