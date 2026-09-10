import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P
import "StationState.js" as StationState

// QML twin of widgets SettingsPage (objectName "settingsPage").
// 三模块卡：🔐账号安全（二级密码设置/修改 + 保护开关三态提示）、
// 🚗车辆管理（列表 + 添加/编辑/删除/设为默认 + 名额提示）、
// 🔔通知与提醒（三 Switch ↔ settingsService.notificationEnabled）。
// 双通道：服务 invokable 可读时以服务为准；桥缺位期落 StationState（.pragma
// library 跨页会话库）——因此密码/车辆/开关在页间往返不丢。
// 二级密码作用点在登录环节（用户二轮指定口径）：本页只负责设置/开关，
// 真正的验证发生在 LoginPage——进入本页不再要求解锁。
// 密码只在库/服务层落哈希，UI 不存任何明文/散列。
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
    // 外观（批次A）：初值即 Service 默认档；reload() 从服务回读真值。
    property string theme: "light"
    property string fontScale: "standard"
    property string palette: "green"

    function call(target, fn, args) {   // 桥缺位期统一吞异常
        try { return target[fn].apply(target, args) } catch (e) { return undefined }
    }
    // 服务 invokable 化探测：vehicles() 能返回数组 = 桥已落地，以服务为准。
    function svcOk() { return call(settingsService, "vehicles", []) !== undefined }
    function reload() {
        // TODO(contract): settingsService.hasProtectionPassword()/protectionEnabled()/vehicles()
        if (svcOk()) {
            page.vehicles = call(settingsService, "vehicles", []) || []
            const hp = call(settingsService, "hasProtectionPassword", [])
            page.hasPassword = (typeof hp === "boolean") ? hp : StationState.hasSecondPassword()
            const pe = call(settingsService, "protectionEnabled", [])
            page.protectionOn = (typeof pe === "boolean") ? pe : StationState.protectionEnabled()
        } else {
            page.vehicles = StationState.vehicles
            page.hasPassword = StationState.hasSecondPassword()
            page.protectionOn = StationState.protectionEnabled()
        }
        // 外观回读（批次A）：桥位必有 theme()/fontScale()（Service 白名单保底）。
        const t = call(settingsService, "theme", [])
        if (typeof t === "string" && t.length > 0) page.theme = t
        const f = call(settingsService, "fontScale", [])
        if (typeof f === "string" && f.length > 0) page.fontScale = f
        const p = call(settingsService, "palette", [])
        if (typeof p === "string" && p.length > 0) page.palette = p
    }
    // 点击→服务（持久化+appearanceChanged→Shell 同步 Style）；按钮选中态
    // 只认服务返回 true，非法值 UI 与服务两侧同口径拒绝。
    function applyTheme(v) {
        if (call(settingsService, "setTheme", [v]) === true) page.theme = v
    }
    function applyFontScale(v) {
        if (call(settingsService, "setFontScale", [v]) === true) page.fontScale = v
    }
    function applyPalette(v) {
        if (call(settingsService, "setPalette", [v]) === true) page.palette = v
    }
    Component.onCompleted: reload()
    Connections {
        target: settingsService
        function onNotificationsChanged() { notifyCol.syncSwitches() }
        function onAppearanceChanged() { reload() }   // 外观卡选中态回读
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

            Text { objectName: "settingsPageTitle"; text: "设置"
                font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.ink }

            // ---- 🔐 账号安全 ----
            P.Card {
                objectName: "settingsSecurityCard"
                width: col.width - col.padding * 2
                Column {
                    width: parent.width
                    spacing: P.Style.spaceSm
                    Text { objectName: "settingsSectionTitle"; text: "账号安全"; font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink }
                    Text {
                        objectName: "protectionPasswordLabel"
                        text: page.hasPassword ? "二级保护密码：已设置" : "二级保护密码：未设置"
                        font.pixelSize: P.Style.fontSm; color: P.Style.muted
                    }
                    P.ActionButton {
                        objectName: "setProtectionPasswordButton"
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
                                StationState.setProtectionEnabled(checked)
                                call(settingsService, "setProtectionEnabled", [checked])
                            }
                        }
                    }
                    Text {
                        objectName: "protectionSwitchHint"
                        width: parent.width; wrapMode: Text.WordWrap
                        text: !page.hasPassword ? "未设置二级保护密码，开关暂不可用——请先点击上方「设置密码」"
                              : page.protectionOn ? "已开启：该账号下次在登录页输入手机号时，将要求输入二级保护密码"
                              : "当前未开启，登录时不做二级密码验证"
                        font.pixelSize: P.Style.fontSm; color: P.Style.faint
                    }
                }
            }

            // ---- 🚗 车辆管理 ----
            P.Card {
                objectName: "settingsVehicleCard"
                width: col.width - col.padding * 2
                Column {
                    width: parent.width
                    spacing: P.Style.spaceSm
                    Text { objectName: "settingsSectionTitle"; text: "车辆管理"; font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink }
                    Text {
                        objectName: "vehiclesEmptyLabel"
                        visible: page.vehicles.length === 0
                        text: "暂无车辆，预约需先添加车辆"
                        font.pixelSize: P.Style.fontSm; color: P.Style.muted
                    }
                    Repeater {
                        model: page.vehicles
                        delegate: Row {
                            objectName: "vehicleCard"
                            width: parent.width
                            spacing: P.Style.spaceXs
                            Column {
                                width: parent.width - 200
                                spacing: 2
                                Row {
                                    spacing: P.Style.spaceXs
                                    Text { text: modelData.plate
                                        font.pixelSize: P.Style.fontMd; font.bold: true; color: P.Style.ink }
                                    P.StatusTag { objectName: "vehicleDefaultTag"; visible: modelData.isDefault; tone: "info"; text: "默认" }
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
                                objectName: "vehicleSetDefaultButton"
                                variant: "chip"; text: "设为默认"
                                visible: !modelData.isDefault
                                onClicked: {
                                    call(settingsService, "setDefaultVehicle", [modelData.id])
                                    if (!svcOk()) StationState.setDefaultVehicle(modelData.id)
                                    reload()
                                }
                            }
                            P.ActionButton {
                                objectName: "vehicleEditButton"
                                variant: "chip"; text: "编辑"
                                onClicked: vehicleDialog.openFor(modelData)
                            }
                            P.ActionButton {
                                objectName: "vehicleDeleteButton"
                                variant: "chip"; text: "删除"
                                onClicked: {
                                    call(settingsService, "removeVehicle", [modelData.id])
                                    if (!svcOk()) StationState.removeVehicle(modelData.id)
                                    reload()
                                }
                            }
                        }
                    }
                    P.ActionButton {
                        objectName: "addVehicleButton"
                        variant: "primary"; text: "＋ 添加车辆"
                        onClicked: vehicleDialog.openFor(null)
                    }
                    Text {
                        objectName: "settingsCaptionLabel"
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
                objectName: "settingsNotificationCard"
                width: col.width - col.padding * 2
                Column {
                    id: notifyCol
                    width: parent.width
                    spacing: P.Style.spaceSm
                    Text { objectName: "settingsSectionTitle"; text: "通知与提醒"; font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink }
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

            // ---- 🎨 外观与字号（2026-09-08 批次A，成员3 追加——本文件已报备）----
            P.Card {
                objectName: "settingsAppearanceCard"
                width: col.width - col.padding * 2
                Column {
                    width: parent.width
                    spacing: P.Style.spaceSm
                    Text { objectName: "settingsSectionTitle"; text: "外观与字号"; font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink }
                    Text { width: parent.width; text: "主题"; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
                    Row {
                        spacing: P.Style.spaceSm
                        P.ActionButton {
                            objectName: "themeLightButton"
                            variant: page.theme === "light" ? "primary" : "secondary"
                            glyph: "sun"
                            text: "浅色"
                            onClicked: page.applyTheme("light")
                        }
                        P.ActionButton {
                            objectName: "themeDarkButton"
                            variant: page.theme === "dark" ? "primary" : "secondary"
                            glyph: "moon"
                            text: "深色"
                            onClicked: page.applyTheme("dark")
                        }
                    }
                    Text { width: parent.width; text: "配色（品牌色板）"; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
                    Row {   // 色板四圆点：色值/名称单源 P.Style.paletteSpecs，页面零字面量
                        spacing: P.Style.spaceLg
                        Repeater {
                            model: P.Style.paletteKeys
                            delegate: Item {   // Column 禁 fill/centerIn 锚（告警严口径）
                                width: 48; height: swatchCol.implicitHeight
                                Column {
                                    id: swatchCol
                                    width: parent.width
                                    spacing: 2
                                    Rectangle {
                                        x: (parent.width - width) / 2
                                        width: 30; height: 30; radius: 15
                                        color: P.Style.paletteSpecs[modelData].brand
                                        border.width: page.palette === modelData ? 3 : 1
                                        border.color: page.palette === modelData
                                            ? P.Style.ink : P.Style.lineStrong
                                    }
                                    Text {
                                        x: (parent.width - implicitWidth) / 2
                                        text: P.Style.paletteSpecs[modelData].label
                                        font.pixelSize: P.Style.fontXs
                                        color: page.palette === modelData ? P.Style.brandDeep : P.Style.muted
                                    }
                                }
                                MouseArea {
                                    objectName: "palette" + modelData.charAt(0).toUpperCase()
                                        + modelData.slice(1) + "Button"
                                    anchors.fill: parent
                                    onClicked: page.applyPalette(modelData)
                                }
                            }
                        }
                    }
                    Text { width: parent.width; text: "字号"; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
                    Row {
                        spacing: P.Style.spaceSm
                        Repeater {
                            model: [
                                { obj: "fontStandardButton",    label: "标准",  key: "standard" },
                                { obj: "fontLargeButton",       label: "大",    key: "large" },
                                { obj: "fontExtraLargeButton",  label: "特大",  key: "extraLarge" }
                            ]
                            delegate: P.ActionButton {
                                objectName: modelData.obj
                                variant: page.fontScale === modelData.key ? "primary" : "secondary"
                                text: modelData.label
                                onClicked: page.applyFontScale(modelData.key)
                            }
                        }
                    }
                    Text {
                        width: parent.width; wrapMode: Text.WordWrap
                        text: "改动即时全应用生效并记住在本机；换配色档后全站品牌色随动（顶部渐变横幅在重新进入页面时翻新）。"
                        font.pixelSize: P.Style.fontSm; color: P.Style.faint
                    }
                }
            }
        }
    }

    // ---- 密码对话框（Popup 直译 widgets QDialog；校验口径同：改密验旧 + ≥4 位 + 两次一致）----
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
            P.TextField {
                id: oldField
                objectName: "currentPasswordEdit"
                width: parent.width; visible: passwordDialog.changing
                placeholderText: "当前密码"
                echoMode: TextInput.Password
            }
            P.TextField {
                id: newField
                objectName: "newPasswordEdit"
                width: parent.width; placeholderText: "新密码（至少 4 位）"
                echoMode: TextInput.Password
            }
            P.TextField {
                id: confirmField
                objectName: "confirmPasswordEdit"
                width: parent.width; placeholderText: "再次输入新密码"
                echoMode: TextInput.Password
            }
            Text {
                objectName: "passwordDialogMessage"
                visible: passwordDialog.note.length > 0
                text: passwordDialog.note; font.pixelSize: P.Style.fontSm; color: P.Style.danger
            }
            Row {
                width: parent.width
                spacing: P.Style.spaceSm
                P.ActionButton {
                    objectName: "passwordCancelButton"
                    variant: "ghost"; text: "取消"
                    width: (parent.width - parent.spacing) / 2
                    onClicked: passwordDialog.close()
                }
                P.ActionButton {
                    objectName: "passwordSaveButton"
                    variant: "primary"; text: "保存密码"
                    width: (parent.width - parent.spacing) / 2
                    onClicked: {
                        if (passwordDialog.changing) {
                            const okSvc = call(settingsService, "verifyProtectionPassword", [oldField.text])
                            const ok = (typeof okSvc === "boolean") ? okSvc : StationState.verifySecondPassword(oldField.text)
                            if (!ok) { passwordDialog.note = "当前密码不正确"; return }
                        }
                        if (newField.text.length < 4) { passwordDialog.note = "密码长度至少 4 位"; return }
                        if (newField.text !== confirmField.text) { passwordDialog.note = "两次输入的密码不一致"; return }
                        // 明文只透传给哈希通道（库/服务），UI 不留存；
                        // 库通道把密码绑定到当前登录手机号——登录页仅对该号码要求验证。
                        // TODO(contract): settingsService.setProtectionPassword(plain) invokable。
                        StationState.setSecondPassword(newField.text,
                            StationState.accountPhone()
                            || (App && App.currentUser && App.currentUser.phone
                                ? App.currentUser.phone : ""))
                        call(settingsService, "setProtectionPassword", [newField.text])
                        page.hasPassword = true
                        passwordDialog.close()
                        if (App) App.showToast("二级保护密码已保存", "success")
                    }
                }
            }
        }
    }

    // ---- 车辆表单（添加/编辑共用；widgets QDialog 直译：车牌/品牌/电池 spin 位/接口单选/默认勾选）----
    Popup {
        id: vehicleDialog
        objectName: "vehicleDialog"
        modal: true
        property var editing: null
        property bool isFast: true
        property bool wantDefault: false
        anchors.centerIn: parent
        width: parent ? Math.min(320, parent.width - P.Style.spaceXl) : 320
        padding: P.Style.spaceLg
        function openFor(vehicle) {
            editing = vehicle || null
            plateField.text = vehicle ? vehicle.plate : ""
            brandField.text = vehicle ? (vehicle.brandModel || "") : ""
            batteryField.text = vehicle ? String(vehicle.batteryKwh || "") : ""
            isFast = !(vehicle && String(vehicle.connectorType).toLowerCase() === "slow")
            wantDefault = vehicle ? !!vehicle.isDefault : page.vehicles.length === 0
            defaultCheck.checked = wantDefault
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
            P.TextField { id: plateField; objectName: "vehiclePlateEdit"
                width: parent.width; placeholderText: "如：粤B·DA1234" }
            Text { width: parent.width; text: "品牌型号"; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
            P.TextField { id: brandField; objectName: "vehicleBrandEdit"
                width: parent.width; placeholderText: "如：比亚迪 汉 EV" }
            Text { width: parent.width; text: "电池容量（kWh）"; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
            // widgets 为 QSpinBox；QML 无同名控件，TextField+IntValidator 等价位（锚点名沿用 spin）。
            P.TextField { id: batteryField; objectName: "vehicleBatterySpin"
                width: parent.width; placeholderText: "如：65"
                validator: IntValidator { bottom: 1; top: 500 } }
            Text { width: parent.width; text: "接口类型"; font.pixelSize: P.Style.fontSm; color: P.Style.muted }
            Row {
                spacing: P.Style.spaceMd
                RadioButton {
                    objectName: "vehicleFastConnectorRadio"
                    text: "快充（直流）"
                    checked: vehicleDialog.isFast
                    onClicked: vehicleDialog.isFast = true
                }
                RadioButton {
                    objectName: "vehicleSlowConnectorRadio"
                    text: "慢充（交流）"
                    checked: !vehicleDialog.isFast
                    onClicked: vehicleDialog.isFast = false
                }
            }
            CheckBox {
                objectName: "vehicleDefaultCheck"
                text: "设为默认车辆（预约时默认选用）"
                onToggled: vehicleDialog.wantDefault = checked
            }
            Text { objectName: "vehicleDialogMessage"; visible: vehicleDialog.note.length > 0
                text: vehicleDialog.note; font.pixelSize: P.Style.fontSm; color: P.Style.danger }
            Row {
                width: parent.width
                spacing: P.Style.spaceSm
                P.ActionButton {
                    objectName: "vehicleCancelButton"
                    variant: "ghost"; text: "取消"
                    width: (parent.width - parent.spacing) / 2
                    onClicked: vehicleDialog.close()
                }
                P.ActionButton {
                    objectName: "vehicleSaveButton"
                    variant: "primary"; text: vehicleDialog.editing ? "保存修改" : "保存车辆"
                    width: (parent.width - parent.spacing) / 2
                    onClicked: {
                        if (plateField.text.trim().length === 0) { vehicleDialog.note = "请填写车牌号码"; return }
                        const v = {
                            id: vehicleDialog.editing ? vehicleDialog.editing.id : 0,
                            plate: plateField.text.trim(),
                            brandModel: brandField.text.trim(),
                            batteryKwh: parseInt(batteryField.text) || 0,
                            connectorType: vehicleDialog.isFast ? "fast" : "slow",
                            isDefault: vehicleDialog.wantDefault || page.vehicles.length === 0
                        }
                        // TODO(contract): addVehicle/updateVehicle invokable（map 载荷）。
                        // 桥缺位期落 StationState 本地通道（跨页可见，预约准入同读此库）。
                        if (vehicleDialog.editing) {
                            call(settingsService, "updateVehicle", [v])
                            if (!svcOk()) StationState.updateVehicle(v)
                        } else {
                            call(settingsService, "addVehicle", [v])
                            if (!svcOk()) {
                                StationState.addVehicle(v)
                                if (v.isDefault && page.vehicles.length > 0)
                                    StationState.setDefaultVehicle(StationState.vehicles[StationState.vehicles.length - 1].id)
                            }
                        }
                        vehicleDialog.close()
                        page.reload()
                        if (App) App.showToast("车辆已保存", "success")
                    }
                }
            }
        }
    }

    // 进页密码门已撤销（用户二轮指定口径）：二级密码的作用点在登录环节，
    // 由 LoginPage 按手机号命中触发验证；本页只做设置与开关。
}
