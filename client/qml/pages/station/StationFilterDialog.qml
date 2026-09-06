import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// QML twin of widgets StationFilterDialog (objectName "stationFilterDialog").
// 8 组条件 = 距离单选（再点=取消）+ 7 组多选 chips（组内 OR，组间 AND 由父页 project() 执行）。
// 选项字面量与 station_query_service.h station_filter::*Options() 逐字一致（字符串即匹配键）。
// TODO(contract): 选项改由服务层 invokable 暴露后，此处删掉常量数组。
Popup {
    id: dialog
    objectName: "stationFilterDialog"
    modal: false
    closePolicy: Popup.CloseOnPressOutside
    width: parent ? Math.min(360, parent.width - P.Style.spaceXl) : 360
    height: parent ? Math.min(460, parent.height - P.Style.spaceXl) : 460
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? (parent.height - height) / 2 : 0
    padding: P.Style.spaceLg

    signal applied(var criteria)

    property var distanceKm: [5, 10, 30, 50]
    property int maxDistanceKm: 0
    property var groups: [
        { key: "statuses",     title: "营业状态",   options: ["营业中", "暂停运营"] },
        { key: "operators",    title: "运营商",     options: ["自营", "合作站", "互联互通", "个人桩"] },
        { key: "accessTypes",  title: "电站类型",   options: ["对外", "不对外开放"] },
        { key: "parkingFees",  title: "停车费",     options: ["免费", "限时免费", "停车减免", "收费"] },
        { key: "features",     title: "特色功能",   options: ["重卡", "即插即充", "CPU即插即充", "有序充电", "V2G"] },
        { key: "chargerTypes", title: "充电桩类型", options: ["超充", "快充", "慢充"] },
        { key: "voltageBands", title: "充电桩电压", options: ["低于700V", "700V及以上"] }
    ]
    property var checked: ({ statuses: [], operators: [], accessTypes: [], parkingFees: [],
                             features: [], chargerTypes: [], voltageBands: [] })

    function isChecked(key, label) { return (checked[key] || []).indexOf(label) >= 0 }
    function toggle(key, label) {          // 数组必须整体重赋值才触发绑定重算
        const next = ({})
        for (const g of groups) next[g.key] = (checked[g.key] || []).slice()
        const a = next[key]
        const i = a.indexOf(label)
        if (i >= 0) a.splice(i, 1); else a.push(label)
        checked = next
    }
    function setDistance(km) { maxDistanceKm = (maxDistanceKm === km ? 0 : km) }
    function resetChecks() {               // 「重置」只清勾选（与 widgets 同口径）
        maxDistanceKm = 0
        const next = ({})
        for (const g of groups) next[g.key] = []
        checked = next
    }
    function openDialog(initial) {         // 以父页当前条件预勾选
        maxDistanceKm = (initial && initial.maxDistanceKm) || 0
        const next = ({})
        for (const g of groups) next[g.key] = ((initial && initial[g.key]) || []).slice()
        checked = next
        open()
    }
    function applyAndClose() {
        applied({
            maxDistanceKm: dialog.maxDistanceKm,
            statuses: checked.statuses, operators: checked.operators,
            accessTypes: checked.accessTypes, parkingFees: checked.parkingFees,
            features: checked.features, chargerTypes: checked.chargerTypes,
            voltageBands: checked.voltageBands
        })
        close()
    }

    background: Rectangle {
        radius: P.Style.radiusLg
        color: P.Style.surface
        border.color: P.Style.line; border.width: 1
    }

    Column {
        anchors.fill: parent
        spacing: P.Style.spaceMd

        ScrollView {
            id: scroller
            width: parent.width
            height: parent.height - footerRow.height - parent.spacing
            clip: true
            Column {
                width: scroller.availableWidth
                spacing: P.Style.spaceMd

                // 距离组（单选）
                Column {
                    objectName: "filterGroupDistance"
                    spacing: P.Style.spaceXs
                    Text { text: "距离"; font.pixelSize: P.Style.fontMd; font.bold: true; color: P.Style.ink }
                    Flow {
                        width: parent.width
                        spacing: P.Style.spaceSm
                        Repeater {
                            model: dialog.distanceKm
                            P.ActionButton {
                                variant: dialog.maxDistanceKm === modelData ? "primary" : "chip"
                                text: modelData + "公里内"
                                onClicked: dialog.setDistance(modelData)
                            }
                        }
                    }
                }

                // 7 组多选
                Repeater {
                    model: dialog.groups
                    delegate: Column {
                        id: groupCol
                        property var group: modelData   // 内层 Repeater 会遮蔽 modelData，先挂到 id 上
                        objectName: "filterGroup" + group.key
                        width: scroller.availableWidth
                        spacing: P.Style.spaceXs
                        Text {
                            text: group.title
                            font.pixelSize: P.Style.fontMd; font.bold: true; color: P.Style.ink
                        }
                        Flow {
                            width: parent.width
                            spacing: P.Style.spaceSm
                            Repeater {
                                model: group.options
                                P.ActionButton {
                                    objectName: "filterChip"
                                    variant: dialog.isChecked(groupCol.group.key, modelData)
                                             ? "primary" : "chip"
                                    text: modelData
                                    onClicked: dialog.toggle(groupCol.group.key, modelData)
                                }
                            }
                        }
                    }
                }
            }
        }

        Row {
            id: footerRow
            objectName: "filterFooter"
            width: parent.width
            spacing: P.Style.spaceSm
            P.ActionButton {
                objectName: "filterCancelButton"
                variant: "ghost"; text: "取消"
                width: (footerRow.width - footerRow.spacing * 2) / 3
                onClicked: dialog.close()
            }
            P.ActionButton {
                objectName: "filterResetButton"
                variant: "secondary"; text: "重置"
                width: (footerRow.width - footerRow.spacing * 2) / 3
                onClicked: dialog.resetChecks()
            }
            P.ActionButton {
                objectName: "filterApplyButton"
                variant: "primary"; text: "确定"
                width: (footerRow.width - footerRow.spacing * 2) / 3
                onClicked: dialog.applyAndClose()
            }
        }
    }
}
