import QtQuick
import QtQuick.Controls.Basic
import QtWebEngine
import "../../platform" as P

// Real Tencent map only. An absent key/network never becomes a fake map.
Rectangle {
    id: panel
    property string html: ""
    property string cityName: "大连市"
    signal stationSelected(string stationId)
    color: P.Style.surface
    radius: P.Style.radiusLg
    border.color: P.Style.line
    clip: true

    function acceptStationLink(url) {
        const prefix = "charging-station://select/"
        const value = String(url)
        if (value.indexOf(prefix) !== 0) return false
        let id = ""
        try { id = decodeURIComponent(value.substring(prefix.length)) } catch (e) { return true }
        if (id.length > 0 && id.length <= 128) panel.stationSelected(id)
        return true
    }

    Loader {
        anchors.fill: parent
        anchors.margins: 1
        active: panel.html.length > 0
        sourceComponent: Component {
            WebEngineView {
                id: web
                objectName: "stationWebMap"
                property string displayedHtml: ""
                property bool awaitingInlineDocument: false
                settings.localContentCanAccessRemoteUrls: true
                settings.localContentCanAccessFileUrls: false
                settings.javascriptCanOpenWindows: false
                function reloadMap() {
                    if (displayedHtml === panel.html) return
                    displayedHtml = panel.html
                    awaitingInlineDocument = true
                    loadHtml(displayedHtml, "https://map.qq.com/")
                }
                Component.onCompleted: reloadMap()
                Connections { target: panel; function onHtmlChanged() { web.reloadMap() } }
                onNavigationRequested: function(request) {
                    const url = String(request.url)
                    // Qt implements loadHtml via a data:text/html navigation.
                    // Permit only our next inline load, not arbitrary web links.
                    if (awaitingInlineDocument && url.indexOf("data:text/html") === 0) {
                        awaitingInlineDocument = false
                        return
                    }
                    if (panel.acceptStationLink(request.url))
                        request.action = WebEngineNavigationRequest.IgnoreRequest
                    else if (url !== "about:blank" && url !== "https://map.qq.com/")
                        request.action = WebEngineNavigationRequest.IgnoreRequest
                }
                // JS SDK errors include request URLs on some versions. Do not
                // echo console messages that might contain a map credential.
                onJavaScriptConsoleMessage: function(level, message, lineNumber, sourceID) {}
            }
        }
    }
    Column {
        visible: panel.html.length === 0
        anchors.centerIn: parent
        width: parent.width - 32
        spacing: 10
        Text {
            width: parent.width; horizontalAlignment: Text.AlignHCenter
            text: panel.cityName + "电站地图"; font.pixelSize: P.Style.fontLg
            font.bold: true; color: P.Style.ink
        }
        Text {
            width: parent.width; horizontalAlignment: Text.AlignHCenter
            text: "配置 TENCENT_MAP_API_KEY 后重启客户端即可加载真实地图。\n地图不可用时仍可从下方列表选桩预约。"
            wrapMode: Text.Wrap; font.pixelSize: P.Style.fontSm; color: P.Style.muted
        }
    }
}
