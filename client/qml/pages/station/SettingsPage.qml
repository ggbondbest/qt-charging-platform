import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// QML twin of widgets SettingsPage (objectName "settingsPage").
// 三模块卡：🔐账号安全（二级密码设置/修改 + 保护开关三态提示）、
// 🚗车辆管理（列表 + 添加/编辑/删除/设为默认 + 名额提示）、
// 🔔通知与提醒（三 Switch ↔ settingsService.notificationEnabled）。
// 服务方法均为裸对象非桥（TODO(contract)：invokable 化，见映射稿 §桥缺口）；
// 密码只在服务层落哈希，UI 不存任何明文/散列。
Item {
    id: page
    objectName: "settingsPage"
    property string route: "settings"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    property bool hasPassword: false
    property bool protectionOn: false
    property var vehicles: []

    function call(target, fn, args) {   // 桥缺位期统一吞异常
        try { return target[fn].apply(target, args) } catch (e) { return undefined }
    }
    function reload() {
        // TODO(contract): settingsService.hasSecondPassword()/protectionEnabled()/vehicles()
        const v = call(settingsService, "vehicles", [])
        page.vehicles = v || []
    }
    Component.onCompleted: reload()
    Connections {
        target: settingsService
        function onNotificationsChanged() { notifyCol.syncSwitches() }
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: col.height
        clip: true

        Column {
            id: col
            width: parent.width
            padding: P.Style.spaceLg
            spacing: P.Style.spaceMd

            Text { text: "设置"; font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.ink }

            // ---- 🔐 账号安全 ----
            P.Card {
                objectName: "accountSecurityCard"
                width: col.width - col.padding * 2
                Column {
                    width: parent.width
                    spacing: P.Style.spaceSm
                    Text { text: "🔐 账号安全"; font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink }
                    Text {
                        objectName: "passwordStatusLabel"
                        text: page.hasPassword ? "二级保护密码：已设置" : "二级保护密码：未设置"
                        font.pixelSize: P.Style.fontSm; color: P.Style.muted
                    }
                    P.ActionButton {
                        objectName: "passwordButton"
                        variant: "secondary"
                        text: page.hasPassword ? "修改密码" : "设置密码"
                        onClicked: passwordDialog.openFor(page.hasPassword)
                    }
                    Row {
                        width: parent.width
                        spacing: P.Style.spaceSm
                        Switch {
                            objectName: "protectionSwitch"
                            anchors.verticalCenter: parent.verticalCenter
                            text: "开启二级保护密码验证"
                            enabled: page.hasPassword
                            checked: page.protectionOn
                            onToggled: {
                                page.protectionOn = checked
                                call(settingsService, "setSecondProtectionEnabled", [checked])
                            }
                        }
                    }
                    Text {
                        width: parent.width; wrapMode: Text.WordWrap
                        text: !page.hasPassword ? "未设置二级保护密码，开关暂不可用——请先点击上方「设置密码」"
                              : page.protectionOn ? "关键操作（预约/取消）将要求输入二级密码"
                              : "当前未开启，关键操作不做二次验证"
                        font.pixelSize: P.Style.fontSm; color: P.Style.faint
                    }
                }
            }

            // ---- 🚗 车辆管理 ----
            P.Card {
                objectName: "vehicleManagementCard"
                width: col.width - col.padding * 2
                Column {
                    width: parent.width
                    spacing: P.Style.spaceSm
                    Text { text: "🚗 车辆管理"; font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink }
                    Text {
                        visible: page.vehicles.length === 0
                        text: "暂无车辆，预约需先添加车辆"
                        font.pixelSize: P.Style.fontSm; color: P.Style.muted
                    }
                    Repeater {
                        model: page.vehicles
                        delegate: Row {
                            width: parent.width
                            spacing: P.Style.spaceXs
                            Column {
                                width: parent.width - 200
                                spacing: 2
                                Row {
                                    spacing: P.Style.spaceXs
                                    Text { text: modelData.plate
                                        font.pixelSize: P.Style.fontMd; font.bold: true; color: P.Style.ink }
                                    P.StatusTag { visible: modelData.isDefault; tone: "info"; text: "默认" }
                                }
                                Text {
                                    width: parent.width; elide: Text.ElideRight
                                    text: (modelData.brandModel || "未填写品牌型号")
                                          + " · 接口：" + (String(modelData.connectorType).toLowerCase() === "fast" ? "快充（直流）" : "慢充（交流）")
                                          + " · 电池：" + (modelData.batteryKwh || 0) + " kWh"
                                    font.pixelSize: P.Style.fontSm; color: P.Style.muted
                                }
                            }
                            P.ActionButton {
                                variant: "chip"; text: "设为默认"
                                visible: !modelData.isDefault
                                onClicked: { call(settingsService, "setDefaultVehicle", [modelData.id]); reload() }
                            }
                            P.ActionButton {
                                variant: "chip"; text: "编辑"
                                onClicked: vehicleDialog.openFor(modelData)
                            }
                            P.ActionButton {
                                objectName: "vehicleDeleteButton"
                                variant: "chip"; text: "删除"
                                onClicked: { call(settingsService, "removeVehicle", [modelData.id]); reload() }
                            }
                        }
                    }
                    P.ActionButton {
                        objectName: "addVehicleButton"
                        variant: "primary"; text: "＋ 添加车辆"
                        onClicked: vehicleDialog.openFor(null)
                    }
                    Text {
                        width: parent.width; wrapMode: Text.WordWrap
                        text: page.vehicles.length === 0
                              ? "当前 0 辆车 → 无法发起预约；添加车辆后即可预约"
                              : "当前 " + page.vehicles.length + " 辆车 → 最多可同时持有 "
                                + page.vehicles.length + " 个有效预约时段（每辆 1 个）"
                        font.pixelSize: P.Style.fontSm; color: P.Style.faint
                    }
                }
            }

            // ---- 🔔 通知与提醒 ----
            P.Card {
                objectName: "notificationSettingsCard"
                width: col.width - col.padding * 2
                Column {
                    id: notifyCol
                    width: parent.width
                    spacing: P.Style.spaceSm
                    Text { text: "🔔 通知与提醒"; font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink }
                    Repeater {
                        model: [
                            { obj: "expiryReminderSwitch",     label: "预约到期提醒", key: "expiry" },
                            { obj: "reservationSuccessSwitch", label: "预约成功通知", key: "success" },
                            { obj: "reservationCancelSwitch",  label: "预约取消通知", key: "cancel" }
                        ]
                        delegate: Switch {
                            objectName: modelData.obj
                            text: modelData.label
                            checked: true
                            onToggled: call(settingsService, "setNotificationEnabled", [modelData.key, checked])
                            Component.onCompleted: {
                                // TODO(contract): notificationEnabled(key) invokable 化后回读真值
                                const v = call(settingsService, "notificationEnabled", [modelData.key])
                                if (typeof v === "boolean") checked = v
                            }
                        }
                    }
                    function syncSwitches() { /* 服务侧变更回推（桥补全后接通知刷新） */ }
                }
            }
        }
    }

    // ---- 密码对话框（Popup 直译 widgets QDialog；校验口径同：≥4 位 + 两次一致）----
    Popup {
        id: passwordDialog
        objectName: "passwordDialog"
        modal: true
        property bool changing: false
        property string note: ""
        anchors.centerIn: parent
        width: parent ? Math.min(320, parent.width - P.Style.spaceXl) : 320
        padding: P.Style.spaceLg
        function openFor(changing_) {
            passwordDialog.changing = changing_; passwordDialog.note = ""
            oldField.text = ""; newField.text = ""; confirmField.text = ""
            open()
        }
        background: Rectangle {
            radius: P.Style.radiusLg; color: P.Style.surface
            border.color: P.Style.line; border.width: 1
        }
        Column {
            width: parent.width
            spacing: P.Style.spaceSm
            Text { text: passwordDialog.changing ? "修改二级保护密码" : "设置二级保护密码"
                font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink }
            TextField {
                id: oldField
                objectName: "oldPasswordField"
                width: parent.width; visible: passwordDialog.changing
                placeholderText: "当前密码"
                echoMode: TextInput.Password
            }
            TextField {
                id: newField
                objectName: "newPasswordField"
                width: parent.width; placeholderText: "新密码（至少 4 位）"
                echoMode: TextInput.Password
            }
            TextField {
                id: confirmField
                objectName: "confirmPasswordField"
                width: parent.width; placeholderText: "再次输入新密码"
                echoMode: TextInput.Password
            }
            Text {
                visible: passwordDialog.note.length > 0
                text: passwordDialog.note; font.pixelSize: P.Style.fontSm; color: P.Style.danger
            }
            Row {
                width: parent.width
                spacing: P.Style.spaceSm
                P.ActionButton {
                    variant: "ghost"; text: "取消"
                    width: (parent.width - parent.spacing) / 2
                    onClicked: passwordDialog.close()
                }
                P.ActionButton {
                    objectName: "passwordSaveButton"
                    variant: "primary"; text: "保存"
                    width: (parent.width - parent.spacing) / 2
                    onClicked: {
                        if (newField.text.length < 4) { passwordDialog.note = "密码长度至少 4 位"; return }
                        if (newField.text !== confirmField.text) { passwordDialog.note = "两次输入的密码不一致"; return }
                        // 明文只透传给服务层（哈希在服务内落 QSettings，UI 不留存）。
                        // TODO(contract): settingsService.setSecondPassword(plain) invokable。
                        call(settingsService, "setSecondPassword", [newField.text])
                        page.hasPassword = true
                        passwordDialog.close()
                        if (App) App.showToast("二级保护密码已保存", "success")
                    }
                }
            }
        }
    }

    // ---- 车辆表单（添加/编辑共用）----
    Popup {
        id: vehicleDialog
        objectName: "vehicleEditDialog"
        modal: true
        property var editing: null
        anchors.centerIn: parent
        width: parent ? Math.min(320, parent.width - P.Style.spaceXl) : 320
        padding: P.Style.spaceLg
        function openFor(vehicle) {
            editing = vehicle || null
            plateField.text = vehicle ? vehicle.plate : ""
            brandField.text = vehicle ? (vehicle.brandModel || "") : ""
            batteryField.text = vehicle ? String(vehicle.batteryKwh || "") : ""
            typeCombo.currentIndex = vehicle && String(vehicle.connectorType).toLowerCase() === "slow" ? 1 : 0
            note = ""
            open()
        }
        property string note: ""
        background: Rectangle {
            radius: P.Style.radiusLg; color: P.Style.surface
            border.color: P.Style.line; border.width: 1
        }
        Column {
            width: parent.width
            spacing: P.Style.spaceSm
            Text { text: vehicleDialog.editing ? "编辑车辆" : "添加车辆"
                font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink }
            Text { width: parent.width; wrapMode: Text.WordWrap
                text: "车牌号码"; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
            TextField { id: plateField; objectName: "vehiclePlateEdit"
                width: parent.width; placeholderText: "如：粤B·DA1234" }
            Text { width: parent.width; text: "品牌型号"; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
            TextField { id: brandField; objectName: "vehicleBrandEdit"
                width: parent.width; placeholderText: "如：比亚迪 汉 EV" }
            Text { width: parent.width; text: "电池容量（kWh）"; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
            TextField { id: batteryField; objectName: "vehicleBatteryEdit"
                width: parent.width; placeholderText: "如：65"
                validator: IntValidator { bottom: 1; top: 500 } }
            Text { width: parent.width; text: "接口类型"; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
            ComboBox {
                id: typeCombo; objectName: "vehicleTypeCombo"
                width: parent.width
                model: ["快充（直流）", "慢充（交流）"]
            }
            Text { visible: vehicleDialog.note.length > 0
                text: vehicleDialog.note; font.pixelSize: P.Style.fontSm; color: P.Style.danger }
            Row {
                width: parent.width
                spacing: P.Style.spaceSm
                P.ActionButton {
                    variant: "ghost"; text: "取消"
                    width: (parent.width - parent.spacing) / 2
                    onClicked: vehicleDialog.close()
                }
                P.ActionButton {
                    objectName: "vehicleSaveButton"
                    variant: "primary"; text: "保存"
                    width: (parent.width - parent.spacing) / 2
                    onClicked: {
                        if (plateField.text.trim().length === 0) { vehicleDialog.note = "请填写车牌号码"; return }
                        const v = {
                            id: vehicleDialog.editing ? vehicleDialog.editing.id : 0,
                            plate: plateField.text.trim(),
                            brandModel: brandField.text.trim(),
                            batteryKwh: parseInt(batteryField.text) || 0,
                            connectorType: typeCombo.currentIndex === 0 ? "fast" : "slow"
                        }
                        // TODO(contract): addVehicle/updateVehicle invokable（map 载荷）。
                        if (vehicleDialog.editing) call(settingsService, "updateVehicle", [v])
                        else call(settingsService, "addVehicle", [v])
                        vehicleDialog.close()
                        page.reload()
                        if (App) App.showToast("车辆已保存", "success")
                    }
                }
            }
        }
    }
}
