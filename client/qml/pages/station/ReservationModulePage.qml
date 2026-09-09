import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// QML twin of widgets ReservationModulePage (objectName "reservationModulePage").
// 两级 Tab：🕒 预约订单（ReservationOrderPage.qml）/ 📒 已完成的预约
// （ReservationCompletedPage.qml），两块子页作哑视图由本页喂 records——
// 避免双 Connections 重复响应服务信号。列表/取消/过期信号在此统一收敛。
Item {
    id: page
    objectName: "reservationModulePage"
    property string route: "reservation_module"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    property int tab: 0                  // 0=预约订单 1=已完成
    property var raw: []
    property bool loading: false
    property bool loaded: false
    property bool failed: false
    property string failMessage: ""

    readonly property var activeRecords:
        (raw || []).filter(r => String(r.status).toLowerCase() === "active")
    readonly property var doneRecords:
        (raw || []).filter(r => String(r.status).toLowerCase() !== "active")
    readonly property var current: activeRecords.length > 0 ? activeRecords[0] : null

    function refresh() {
        loading = true; failed = false
        try { reservationService.fetchList() }
        catch (e) {
            loading = false; loaded = false; failed = true
            failMessage = "预约服务不可用，请重新登录后重试"
            raw = []
        }
    }
    property bool demo: false
    Connections {
        target: reservationService
        function onListStarted() { page.loading = true; page.failed = false }
        function onListSucceeded(records) {
            page.loading = false; page.loaded = true; page.failed = false
            page.demo = false                       // 真数据到位，演示记录退位
            page.raw = records || []
        }
        function onListFailed(message) {
            page.loading = false; page.loaded = false; page.failed = true
            page.failMessage = message
        }
        function onCancelSucceeded(reservationId) {
            page.tab = 1        // 取消成功后切到归档页（widgets 同行为）
            page.refresh()
        }
        function onReservationExpired(reservationId) { page.refresh() }
    }
    Component.onCompleted: refresh()

    // 整页可上下拖拽（用户二轮指定）：头部随页滚动，子页列表在自身视口内滚动。
    Flickable {
        id: moduleFlick
        anchors.fill: parent
        anchors.margins: P.Style.spaceLg
        contentWidth: width
        contentHeight: moduleCol.height
        clip: true

        Column {
            id: moduleCol
            width: moduleFlick.width
            spacing: P.Style.spaceMd

            Text { objectName: "reservationModuleTitle"; text: "我的预约"
                font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.ink }

            // 二级 Tab
            Row {
                objectName: "reservationTabs"
                spacing: P.Style.spaceSm
                P.ActionButton {
                    objectName: "reservationOrderTabButton"
                    variant: "chip"
                    selected: page.tab === 0
                    text: "进行中"
                    onClicked: page.tab = 0
                }
                P.ActionButton {
                    objectName: "reservationHistoryTabButton"
                    variant: "chip"
                    selected: page.tab === 1
                    text: "预约历史"
                    onClicked: page.tab = 1
                }
            }

            Text {
                objectName: "moduleDemoCaption"
                visible: page.demo
                width: parent.width
                wrapMode: Text.WordWrap      // NoWrap 长标注会溢出裁字
                text: "当前为演示记录（预约桥未就绪，接入后自动替换）"
                font.pixelSize: P.Style.fontSm; color: P.Style.faint
            }

            // 整页失败态（子页不再叠加错误横幅）——保留原失败提示 UI。
            P.NoticePanel {
                objectName: "moduleNotice"
                width: parent.width
                height: 180
                visible: page.failed
                glyph: "⚠️"
                title: "预约列表加载失败"
                description: page.failMessage
                actionText: "重试"
                onActionTriggered: page.refresh()
            }

            // 子页装载：Loader 保单一活动视图
            Loader {
                objectName: "reservationTabContent"
                width: parent.width
                height: page.failed ? 0
                                     : Math.max(320, moduleFlick.height - y - P.Style.spaceMd)
                active: !page.failed
                source: page.tab === 0 ? "ReservationOrderPage.qml"
                                       : "ReservationCompletedPage.qml"
                onLoaded: {
                    if (!item) return
                    if (page.tab === 0) {
                        item.record = Qt.binding(() => page.current)
                        item.loading = Qt.binding(() => page.loading && !page.loaded)
                        item.parentFailed = Qt.binding(() => false)
                    } else {
                        item.records = Qt.binding(() => page.doneRecords)
                        item.loading = Qt.binding(() => page.loading && !page.loaded)
                    }
                }
            }
        }
    }
}
