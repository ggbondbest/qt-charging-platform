# pages/station/ — owned by member 2 agent. Append YOUR migrated pages below
# (one path per line, e.g. ${CMAKE_CURRENT_SOURCE_DIR}/StationHomePage.qml);
# do not touch anything outside this directory.
set(CHARGING_QML_FILES ${CHARGING_QML_FILES}
    ${CMAKE_CURRENT_SOURCE_DIR}/LoginPage.qml
    ${CMAKE_CURRENT_SOURCE_DIR}/StationHomePage.qml
    ${CMAKE_CURRENT_SOURCE_DIR}/StationDetailPage.qml
    ${CMAKE_CURRENT_SOURCE_DIR}/ReservationConfirmPage.qml
    ${CMAKE_CURRENT_SOURCE_DIR}/ProfilePage.qml
    ${CMAKE_CURRENT_SOURCE_DIR}/StationMapItem.qml
    ${CMAKE_CURRENT_SOURCE_DIR}/StationFilterDialog.qml
    PARENT_SCOPE)
