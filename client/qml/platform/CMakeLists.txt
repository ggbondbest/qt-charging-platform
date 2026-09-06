# platform/ — owned by member 3 agent. Others: READ ONLY.
set(CHARGING_QML_FILES ${CHARGING_QML_FILES}
    ${CMAKE_CURRENT_SOURCE_DIR}/Style.qml
    ${CMAKE_CURRENT_SOURCE_DIR}/qmldir
    ${CMAKE_CURRENT_SOURCE_DIR}/ActionButton.qml
    ${CMAKE_CURRENT_SOURCE_DIR}/ActionBar.qml
    ${CMAKE_CURRENT_SOURCE_DIR}/BottomTabBar.qml
    ${CMAKE_CURRENT_SOURCE_DIR}/Card.qml
    ${CMAKE_CURRENT_SOURCE_DIR}/ClickableCard.qml
    ${CMAKE_CURRENT_SOURCE_DIR}/LoadingOverlay.qml
    ${CMAKE_CURRENT_SOURCE_DIR}/NoticePanel.qml
    ${CMAKE_CURRENT_SOURCE_DIR}/PullToRefreshArea.qml
    ${CMAKE_CURRENT_SOURCE_DIR}/StatusTag.qml
    ${CMAKE_CURRENT_SOURCE_DIR}/Toast.qml
    ${CMAKE_CURRENT_SOURCE_DIR}/TopNavBar.qml
    PARENT_SCOPE)
