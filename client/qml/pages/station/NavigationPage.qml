import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtWebEngine
import "../../platform" as P

Item {
    id: page
    objectName: "navigationPage"
    property string route: "navigation"
    property var arg: ({})
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600
    readonly property var record: page.arg || ({})
    property string mode: "driving"
    property string webError: ""
    property string localError: ""
    property string loadedHtml: ""
    property bool ready: false
    property bool locating: false
    property bool ownsRoute: false
    property bool managedByStack: false
    // A standalone preview has no StackView. In a real navigation stack only
    // the active page may own the shared MapBridge request/result channel.
    readonly property bool isActivePage: StackView.view ? StackView.status === StackView.Active
                                                       : !managedByStack
    StackView.onViewChanged: if (StackView.view) managedByStack = true
    readonly property var cities: ["大连市", "沈阳市", "北京市", "上海市", "深圳市"]
    readonly property string destinationName: record.stationName || record.name || "充电站"

    function targetCoordinate(primary, fallback) {
        if (record.hasStationLocation === false) return NaN
        const value = record[primary] !== undefined ? record[primary] : record[fallback]
        return typeof value === "number" && isFinite(value) ? value : NaN
    }
    function requestRoute() {
        if (!ready || !isActivePage || locating || mapBridge.busy) return
        localError = ""
        webError = ""
        if (!mapBridge.hasLocation) {
            localError = "请填写城市和起始地址，点击定位后再规划路线"
            return
        }
        const latitude = targetCoordinate("stationLatitude", "latitude")
        const longitude = targetCoordinate("stationLongitude", "longitude")
        if (!isFinite(latitude) || !isFinite(longitude)
                || latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180) {
            localError = "该电站缺少有效坐标，无法规划路线；请返回电站列表重新选择"
            releaseOwnedRoute()
            return
        }
        ownsRoute = true
        mapBridge.requestRoute(latitude, longitude, mode)
    }
    function locateOrigin() {
        if (!ready || !isActivePage || locating || mapBridge.busy) return
        const city = regionInput.editText.trim()
        const detail = originField.text.trim()
        if (!city.length || !detail.length) {
            localError = "请填写城市和详细起始地址，例如：大连市 · 高新区软件园路"
            return
        }
        localError = ""
        webError = ""
        locating = true
        ownsRoute = true
        routeTimer.stop()
        mapBridge.geocodeAddress(detail.indexOf(city) === 0 ? detail : city + " " + detail)
    }
    function syncRouteHtml() {
        if (!ready || !isActivePage) return
        const html = mapBridge.routeHtml || ""
        // Progress/error notifications must not flash or reload an unchanged map.
        if (html === loadedHtml) return
        loadedHtml = html
        webError = ""
        if (mapLoader.item) mapLoader.item.loadRoute()
    }
    function fillOriginEditor() {
        // Entering navigation before locating must inherit the city currently
        // browsed on Home, not prepend the default Dalian to another city's address.
        if (!mapBridge.hasLocation) {
            const index = cities.indexOf(mapBridge.browsingCity || cities[0])
            regionInput.currentIndex = index >= 0 ? index : 0
            return
        }
        const label = mapBridge.locationLabel || ""
        for (var i = 0; i < cities.length; ++i) {
            if (label.indexOf(cities[i]) === 0) {
                regionInput.currentIndex = i
                originField.text = label.slice(cities[i].length).trim()
                return
            }
        }
    }
    function releaseOwnedRoute() {
        routeTimer.stop()
        locating = false
        loadedHtml = ""
        // An inactive page's later destruction must not cancel its successor.
        if (ownsRoute) {
            ownsRoute = false
            if (typeof mapBridge !== "undefined" && mapBridge) mapBridge.cancelRoute()
        }
    }

    // Coalesce coordinate and final address-label notifications into one request.
    Timer {
        id: routeTimer
        interval: 0; repeat: false
        onTriggered: page.requestRoute()
    }
    Connections {
        target: mapBridge
        function onLocationChanged() {
            if (!page.ready || !page.isActivePage || !mapBridge.hasLocation) return
            page.locating = false
            page.localError = ""
            routeTimer.restart()
        }
        function onChanged() {
            if (!page.ready || !page.isActivePage) return
            if (page.locating && !mapBridge.busy && mapBridge.error.length)
                page.locating = false
            page.syncRouteHtml()
        }
    }
    Component.onCompleted: {
        if (StackView.view) managedByStack = true
        ready = true
        if (!isActivePage) return
        fillOriginEditor()
        syncRouteHtml()
        if (mapBridge.hasLocation) routeTimer.restart()
    }
    Component.onDestruction: {
        ready = false
        releaseOwnedRoute()
    }
    onArgChanged: if (ready && isActivePage) routeTimer.restart()
    onIsActivePageChanged: {
        if (!ready) return
        if (!isActivePage) releaseOwnedRoute()
        else {
            fillOriginEditor()
            syncRouteHtml()
            if (mapBridge.hasLocation) routeTimer.restart()
        }
    }

    Rectangle { anchors.fill: parent; color: P.Style.bg }
    Flickable {
        id: scroll
        objectName: "navigationScroll"
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: content.height + 2 * P.Style.spaceLg
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { }

        // Compact windows scroll instead of compressing/overlapping the map,
        // route steps and buttons. Extra height belongs to the real map.
        ColumnLayout {
            id: content
            x: P.Style.spaceLg; y: P.Style.spaceLg
            width: scroll.width - 2 * P.Style.spaceLg
            height: Math.max(implicitHeight, scroll.height - 2 * P.Style.spaceLg)
            spacing: P.Style.spaceMd

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4
                Text { text: "路线导航"; font.pixelSize: P.Style.fontXs; color: P.Style.muted }
                Text {
                    objectName: "navigationPageTitle"
                    Layout.fillWidth: true
                    text: "前往 " + page.destinationName
                    textFormat: Text.PlainText; wrapMode: Text.WordWrap
                    font.pixelSize: P.Style.fontLg2; font.bold: true; color: P.Style.ink
                }
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: originForm.implicitHeight + 20
                color: P.Style.surface; radius: P.Style.radiusMd; border.color: P.Style.line
                ColumnLayout {
                    id: originForm
                    anchors.fill: parent; anchors.margins: 10
                    spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        P.ComboBox {
                            id: regionInput
                            objectName: "navigationRegionInput"
                            Layout.preferredWidth: Math.max(96, P.Style.fontMd * 5.5)
                            editable: true; model: page.cities
                            enabled: !page.locating && !mapBridge.busy
                            onAccepted: page.locateOrigin()
                        }
                        P.TextField {
                            id: originField
                            objectName: "originField"
                            Layout.fillWidth: true; Layout.minimumWidth: 80
                            placeholderText: "输入详细起始地址"
                            enabled: !page.locating && !mapBridge.busy
                            onAccepted: page.locateOrigin()
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Text {
                            objectName: "navigationOriginLabel"
                            Layout.fillWidth: true
                            text: mapBridge.hasLocation ? "当前起点：" + mapBridge.locationLabel
                                                        : "输入地址定位，无需设备 GPS"
                            textFormat: Text.PlainText; elide: Text.ElideRight
                            font.pixelSize: P.Style.fontXs; color: P.Style.muted
                        }
                        P.ActionButton {
                            objectName: "navigationLocateButton"
                            text: page.locating ? "定位中…" : "定位起点"
                            Layout.preferredHeight: 36
                            enabled: !page.locating && !mapBridge.busy
                            onClicked: page.locateOrigin()
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: P.Style.spaceSm
                P.ActionButton {
                    objectName: "drivingRouteButton"
                    variant: "chip"; selected: page.mode === "driving"; text: "驾车"
                    Layout.fillWidth: true
                    enabled: !page.locating && !mapBridge.busy
                    onClicked: {
                        if (page.mode !== "driving") { page.mode = "driving"; page.requestRoute() }
                    }
                }
                P.ActionButton {
                    objectName: "walkingRouteButton"
                    variant: "chip"; selected: page.mode === "walking"; text: "步行"
                    Layout.fillWidth: true
                    enabled: !page.locating && !mapBridge.busy
                    onClicked: {
                        if (page.mode !== "walking") { page.mode = "walking"; page.requestRoute() }
                    }
                }
                P.ActionButton {
                    objectName: "navigationReplanButton"
                    variant: "secondary"; text: "重新规划"
                    enabled: mapBridge.hasLocation && !page.locating && !mapBridge.busy
                    onClicked: page.requestRoute()
                }
            }
            Text {
                objectName: "navigationCaptionLabel"
                Layout.fillWidth: true
                text: page.localError.length ? page.localError
                      : mapBridge.error.length ? mapBridge.error
                      : page.webError.length ? page.webError
                      : page.locating ? "正在解析起始地址…"
                      : mapBridge.busy ? "正在规划腾讯地图路线…"
                      : mapBridge.routeDistanceMeters >= 0
                        ? (page.mode === "walking" ? "步行" : "驾车") + " · "
                          + (mapBridge.routeDistanceMeters / 1000).toFixed(1)
                          + " km · 约 " + mapBridge.durationMinutes + " 分钟"
                        : "请先定位起点，再获取前往电站的真实路线"
                textFormat: Text.PlainText; wrapMode: Text.WordWrap
                color: page.localError.length || mapBridge.error.length || page.webError.length
                       ? P.Style.danger : P.Style.brandDeep
                font.pixelSize: P.Style.fontSm
            }
            Rectangle {
                objectName: "navigationMapCard"
                Layout.fillWidth: true; Layout.fillHeight: true
                Layout.minimumHeight: 200; Layout.preferredHeight: 250
                color: P.Style.surface; radius: P.Style.radiusMd; border.color: P.Style.line
                clip: true
                Loader {
                    id: mapLoader
                    anchors.fill: parent; anchors.margins: 1
                    active: page.loadedHtml.length > 0 && page.isActivePage
                    visible: page.webError.length === 0
                    sourceComponent: Component {
                        WebEngineView {
                            id: web
                            objectName: "navigationMapPanel"
                            property string displayedHtml: ""
                            property bool inlineDocumentPending: false
                            settings.localContentCanAccessRemoteUrls: true
                            settings.localContentCanAccessFileUrls: false
                            settings.javascriptCanOpenWindows: false
                            function loadRoute() {
                                if (displayedHtml === page.loadedHtml) return
                                displayedHtml = page.loadedHtml
                                inlineDocumentPending = true
                                loadHtml(displayedHtml, "https://map.qq.com/")
                            }
                            Component.onCompleted: loadRoute()
                            onLoadingChanged: function(info) {
                                if (info.status === WebEngineView.LoadSucceededStatus)
                                    inlineDocumentPending = false
                                if (info.status === WebEngineView.LoadFailedStatus && page.loadedHtml.length > 0)
                                    page.webError = "地图加载失败，请检查网络和 JavaScript 地图 Key 授权"
                            }
                            onNavigationRequested: function(request) {
                                // loadHtml uses a data: document internally on
                                // supported Qt versions. Permit that one load,
                                // but not a later arbitrary data: navigation.
                                if (inlineDocumentPending
                                    && String(request.url).indexOf("data:text/html") === 0) {
                                    inlineDocumentPending = false
                                    return
                                }
                                if (String(request.url) !== "about:blank"
                                    && String(request.url) !== "https://map.qq.com/")
                                    request.action = WebEngineNavigationRequest.IgnoreRequest
                            }
                            // SDK diagnostics can contain credential-bearing URLs.
                            onJavaScriptConsoleMessage: function(level, message, lineNumber, sourceID) {}
                        }
                    }
                }
                Column {
                    visible: page.loadedHtml.length === 0 || page.webError.length > 0
                    anchors.centerIn: parent
                    width: parent.width - 32; spacing: 8
                    Text {
                        width: parent.width
                        text: mapBridge.busy ? "正在获取路线" : "路线地图"
                        horizontalAlignment: Text.AlignHCenter
                        font.pixelSize: P.Style.fontLg; font.bold: true; color: P.Style.ink
                    }
                    Text {
                        width: parent.width
                        text: page.webError.length ? page.webError
                              : "定位成功后显示腾讯道路地图及路线，未成功取得路线时不绘制模拟路径"
                        horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                        font.pixelSize: P.Style.fontSm; color: P.Style.muted
                    }
                }
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: stepContent.implicitHeight + 20
                color: P.Style.surface; radius: P.Style.radiusMd; border.color: P.Style.line
                ColumnLayout {
                    id: stepContent
                    anchors.fill: parent; anchors.margins: 10
                    spacing: 8
                    Text {
                        text: "路线指引"; font.pixelSize: P.Style.fontMd
                        font.bold: true; color: P.Style.ink
                    }
                    ListView {
                        id: stepList
                        objectName: "navigationStepsList"
                        Layout.fillWidth: true
                        Layout.preferredHeight: count > 0 ? 116 : 28
                        clip: true; model: mapBridge.steps
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar { }
                        delegate: Item {
                            required property var modelData
                            required property int index
                            width: stepList.width
                            implicitHeight: Math.max(36, instruction.implicitHeight + 14)
                            RowLayout {
                                anchors.fill: parent
                                anchors.topMargin: 7; anchors.bottomMargin: 7
                                spacing: 8
                                Text {
                                    text: String(index + 1) + "."
                                    Layout.alignment: Qt.AlignTop; Layout.preferredWidth: 24
                                    font.pixelSize: P.Style.fontSm; color: P.Style.brandDeep
                                }
                                Text {
                                    id: instruction
                                    Layout.fillWidth: true
                                    text: modelData.instruction || ""
                                    textFormat: Text.PlainText; wrapMode: Text.WordWrap
                                    font.pixelSize: P.Style.fontSm; color: P.Style.ink
                                }
                            }
                        }
                        Text {
                            visible: stepList.count === 0
                            width: parent.width
                            text: "暂无路线指引"; font.pixelSize: P.Style.fontSm; color: P.Style.muted
                        }
                    }
                }
            }
            P.ActionButton {
                objectName: "navigationBackButton"
                Layout.fillWidth: true
                variant: "secondary"; text: "返回电站"
                onClicked: App.back()
            }
        }
    }
}
