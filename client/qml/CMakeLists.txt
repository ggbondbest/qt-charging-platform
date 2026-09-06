# QML migration stack — strictly ADDITIVE. Nothing here references the frozen
# widgets app; the widgets demo remains the default until the cutover PR lands.
# Gated by CHARGING_HAS_QML (root CMake: Quick dev packages found + option ON).

# Owned component/page lists are accumulated from the subdirectories below.
# Each subdirectory appends its .qml files via PARENT_SCOPE — add your page to
# the CMakeLists.txt inside YOUR directory only.
set(CHARGING_QML_FILES "")
add_subdirectory(platform)
add_subdirectory(pages/profile_charging)
add_subdirectory(pages/station)

add_executable(charging_qml_preview
    main.cpp
    ${CHARGING_QML_FILES}
)
set_target_properties(charging_qml_preview PROPERTIES
    OUTPUT_NAME "charging-qml-preview"
)
target_compile_definitions(charging_qml_preview PRIVATE
    CHARGING_QML_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(charging_qml_preview PRIVATE
    ChargingPlatform::ProjectOptions
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
)
