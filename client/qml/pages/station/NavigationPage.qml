import QtQuick
import QtQuick.Controls.Basic
import QtWebEngine
import "../../platform" as P

Item {
    id: page
    objectName: "navigationPage"
    property string route: "navigation"
    property var arg: ({})
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600
    property var record: page.arg || ({})
    property string mode: "driving"
    property string webError: ""

    function requestRoute() {
        webError = ""
        const lat = record.stationLatitude !== undefined ? record.stationLatitude : record.latitude
        const lng = record.stationLongitude !== undefined ? record.stationLongitude : record.longitude
        mapBridge.requestRoute(typeof lat === "number" ? lat : NaN,
                               typeof lng === "number" ? lng : NaN, mode)
    }
    Connections {
        target: mapBridge
        function onChanged() {
            if (mapBridge.routeHtml.length > 0)
                web.loadHtml(mapBridge.routeHtml, "https://map.qq.com/")
        }
    }
    Component.onCompleted: requestRoute()
    Component.onDestruction: mapBridge.cancelRoute()
    onArgChanged: requestRoute()

    Rectangle { anchors.fill: parent; color: P.Style.bg }
    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceLg
        spacing: P.Style.spaceMd
        Text {
            objectName: "navigationPageTitle"
            text: "前往 " + (page.record.stationName || page.record.name || "充电站")
            width: parent.width; wrapMode: Text.WordWrap
            font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.ink
        }
        Text {
            width: parent.width; wrapMode: Text.WordWrap
            text: "起点：" + (mapBridge.hasLocation ? mapBridge.locationLabel : "请返回附近页面输入地址定位")
            color: P.Style.muted; font.pixelSize: P.Style.fontSm
        }
        Row {
            spacing: P.Style.spaceSm
            P.ActionButton {
                objectName: "drivingRouteButton"
                variant: "chip"; selected: page.mode === "driving"; text: "驾车"
                onClicked: { page.mode = "driving"; page.requestRoute() }
            }
            P.ActionButton {
                objectName: "walkingRouteButton"
                variant: "chip"; selected: page.mode === "walking"; text: "步行"
                onClicked: { page.mode = "walking"; page.requestRoute() }
            }
            P.ActionButton { variant: "ghost"; text: "重新规划"; enabled: !mapBridge.busy; onClicked: page.requestRoute() }
        }
        Text {
            objectName: "navigationCaptionLabel"
            width: parent.width; wrapMode: Text.WordWrap
            text: mapBridge.error.length ? mapBridge.error : page.webError.length ? page.webError
                  : mapBridge.busy ? "正在请求腾讯地图真实路线…"
                  : mapBridge.routeDistanceMeters >= 0
                    ? "腾讯地图 · " + (mapBridge.routeDistanceMeters / 1000).toFixed(1)
                      + " km · 约 " + mapBridge.durationMinutes + " 分钟"
                    : "未取得路线，不展示模拟路径或虚构预计时间"
            color: mapBridge.error.length || page.webError.length ? P.Style.danger : P.Style.brandDeep
            font.pixelSize: P.Style.fontSm
        }
        WebEngineView {
            id: web
            objectName: "navigationMapPanel"
            width: parent.width
            height: Math.max(120, parent.height - y - 170)
            visible: mapBridge.routeHtml.length > 0
            settings.localContentCanAccessRemoteUrls: true
            onLoadingChanged: function(info) {
                if (info.status === WebEngineView.LoadFailedStatus)
                    page.webError = "地图页面加载失败，请检查网络和 JavaScript 地图密钥授权"
            }
        }
        ListView {
            objectName: "navigationStepsList"
            width: parent.width; height: 66; clip: true
            model: mapBridge.steps
            delegate: Text {
                width: ListView.view.width
                text: modelData.instruction
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                font.pixelSize: P.Style.fontSm; color: P.Style.ink
            }
        }
        P.ActionButton { objectName: "navigationBackButton"; width: parent.width; variant: "secondary"; text: "返回"; onClicked: App.back() }
    }
}
