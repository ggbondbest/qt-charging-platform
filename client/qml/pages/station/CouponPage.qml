import QtQuick
import "../../platform" as P
import "../../platform/Glyphs.js" as Glyphs

// 优惠券页（新增功能，用户指定：与「设置」入口并列、UI 同款式）。
// 旧版仅在 README 口径里留了"同款式敬请期待"占位槽、从未实装页面——本页为实装。
// 券服务不在 CONTRACT.md（TODO(contract)：couponService.list()/redeem(id) + couponsChanged）；
// 桥缺位期以页内演示数据渲染三态（可用/已使用/已过期），接入后自动替换。
// 演示数据与 mock 直登（App.login）同口径：标"演示数据"，不冒充真实券。
// 答辩补注：route="coupon"；入口=「我的」页「优惠券」行（与「设置」并列）。本页只读桥缓存、
// 不主动拉新：进页强制补拉由 QmlApp::navigate() 漏斗统一做（审查 P2#3——只读缓存同会话会
// 过期：充值发券/停止支付落通知后须重拉），页内仅经 onCouponsChanged 被动重渲染。
Item {
    id: page
    objectName: "couponPage"
    property string route: "coupon"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    // items=券快照（桥真券或演示集）；demo 旗标参与一切用户可见文案——演示数据永不冒充真券。
    property var items: []
    property bool demo: false
    property int tab: 0        // 0=可用 1=已使用 2=已过期
    // 页根对外投影（当前页内无消费位，留给调试/外层读；测试钉走 availableCount 等派生属性）。
    readonly property string tabName: ["available", "used", "expired"][page.tab]

    // 双通道取数：桥就绪→服务端券；缺位/未注入→页内演示五张（字段与上方 TODO shape 对齐，换真桥页面零改动）。
    function load() {
        // TODO(contract): couponService.coupons() invokable → [{id,title,kind,valueCents,
        //                discountTenths,thresholdCents,condition,expiresAtUtc,status,source}]。
        try {
            if (typeof couponService !== "undefined" && couponService) {
                const v = couponService.coupons()
                if (v) { page.items = v; page.demo = false; return }
            }
        } catch (e) { /* 桥缺位 → 演示数据 */ }
        // 演示五张：三态全覆盖 × 两面额型（cash/discount），到期日用"现在±N天"构造，保证演示态永远自洽。
        const now = Date.now(), day = 86400000
        page.items = [
            { id: 1, kind: "cash", title: "新客立减券", valueCents: 1000,
              condition: "充电满 ¥20 可用", expiresAtUtc: now + 6 * day,
              status: "available", source: "平台新客礼" },
            { id: 2, kind: "discount", title: "充电折扣券", discountTenths: 88,
              condition: "单笔最高抵 ¥5", expiresAtUtc: now + 3 * day,
              status: "available", source: "充值回馈" },
            { id: 3, kind: "cash", title: "服务费减免券", valueCents: 500,
              condition: "服务费满 ¥5 可用", expiresAtUtc: now + 14 * day,
              status: "available", source: "活动发放" },
            { id: 4, kind: "cash", title: "节日充电券", valueCents: 2000,
              condition: "满 ¥30 可用", expiresAtUtc: now - 2 * day,
              status: "used", source: "节日活动" },
            { id: 5, kind: "cash", title: "早鸟体验券", valueCents: 800,
              condition: "无门槛", expiresAtUtc: now - 5 * day,
              status: "expired", source: "拉新奖励" }
        ]
        page.demo = true
    }
    // 被动刷新口：navigate 漏斗的补拉在桥侧发出，回话经 couponsChanged 打到这里（见文件头），页内无轮询。
    Connections {
        target: page.couponSvc
        function onCouponsChanged() { page.load() }
    }
    // 上下文属性可能不存在（券服务未注册）：typeof 守卫取值，避免裸引用 ReferenceError。
    // 依赖属性允许"先引用后声明"：QML 按对象成员解析，不按文本顺序。
    readonly property var couponSvc: (typeof couponService !== "undefined") ? couponService : null
    // 建页只读一次缓存；要不要新数据由漏斗补拉时机决定。
    Component.onCompleted: load()

    // 三个 tab 共用一个过滤口；status 统一小写比较，兼容服务端大小写差异。
    function rowsFor(tab) {
        const name = ["available", "used", "expired"][tab]
        return page.items.filter(function (c) {
            return String(c.status).toLowerCase() === name
        })
    }
    // 面额两型两种显示：cash 分→¥ 取整；discount 十分之一折→"X.X折"（88→8.8折）。
    function faceText(c) {
        return c.kind === "discount"
             ? ((c.discountTenths || 0) / 10).toFixed(1) + "折"
             : "¥" + ((c.valueCents || 0) / 100).toFixed(0)
    }
    // 非数值（服务端未给到期字段）显示 "--"，不拿当前时间假装算出一个到期日。
    function dateText(v) {
        if (typeof v !== "number" || isNaN(v)) return "--"
        const d = new Date(v)
        return d.getFullYear() + "-" + ("0" + (d.getMonth() + 1)).slice(-2)
             + "-" + ("0" + d.getDate()).slice(-2) + " 到期"
    }
    // 派生计数：供"可用 N"chip 文案，也是 offscreen 测试钉（delegate 行数在测试里不可靠）。
    readonly property int availableCount: rowsFor(0).length

    Column {
        anchors.fill: parent
        anchors.margins: P.Style.spaceLg
        spacing: P.Style.spaceMd

        // ---- 标题 + 诚实说明行（demo 旗标直接进文案） ----
        Text { objectName: "couponPageTitle"; text: "优惠券"
            font.pixelSize: P.Style.fontXl; font.bold: true; color: P.Style.ink }
        Text {
            objectName: "couponPageCaption"
            width: parent.width; wrapMode: Text.WordWrap
            text: "充电结算时自动可选抵扣券；"
                  + (page.demo ? "当前为演示数据（券服务桥未就绪，接入后自动替换）"
                               : "数据来自券服务")
            font.pixelSize: P.Style.fontSm; color: P.Style.muted
        }

        // ---- 状态 tabs（页内单值切换，无路由；选中态=primary，其余=chip） ----
        Row {
            objectName: "couponTabs"
            width: parent.width
            spacing: P.Style.spaceSm
            P.ActionButton {
                objectName: "couponAvailableTabButton"
                variant: page.tab === 0 ? "primary" : "chip"
                text: "可用 " + page.availableCount
                onClicked: { page.tab = 0 }
            }
            P.ActionButton {
                objectName: "couponUsedTabButton"
                variant: page.tab === 1 ? "primary" : "chip"
                text: "已使用"
                onClicked: { page.tab = 1 }
            }
            P.ActionButton {
                objectName: "couponExpiredTabButton"
                variant: page.tab === 2 ? "primary" : "chip"
                text: "已过期"
                onClicked: { page.tab = 2 }
            }
        }

        // ---- 券卡列表（与空态面板互斥：当前 tab 有券才显示） ----
        ListView {
            objectName: "couponList"
            width: parent.width
            height: parent.height - y
            clip: true
            spacing: P.Style.spaceSm
            visible: page.rowsFor(page.tab).length > 0
            model: page.rowsFor(page.tab)
            delegate: P.Card {
                objectName: "couponCard"
                width: parent.width
                border.color: modelData.status === "available" ? P.Style.brandSoft : P.Style.line
                Row {
                    width: parent.width            // Card 内容进 Column 容器：anchors 被忽略且告警
                    spacing: P.Style.spaceMd
                    // 面额块（左）
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 76; height: 58; radius: P.Style.radiusMd
                        color: modelData.status === "available" ? P.Style.brandSoft : P.Style.bg
                        Text {
                            anchors.centerIn: parent
                            text: page.faceText(modelData)
                            font.pixelSize: P.Style.fontXl; font.bold: true
                            color: modelData.status === "available" ? P.Style.brandDeep : P.Style.faint
                        }
                    }
                    // 名称/门槛/来源（中）
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        // 剩余宽=行宽−左面额块76−右动作位96−两个间距（三列 Row 布局，中列吃掉弹性）。
                        width: parent.width - 76 - 96 - parent.spacing * 2
                        spacing: 2
                        Text { width: parent.width; elide: Text.ElideRight
                            text: modelData.title || ""
                            font.pixelSize: P.Style.fontMd; font.bold: true; color: P.Style.ink }
                        Text { width: parent.width; elide: Text.ElideRight
                            text: (modelData.condition || "") + " · " + (modelData.source || "")
                            font.pixelSize: P.Style.fontSm; color: P.Style.muted }
                        Text { text: page.dateText(modelData.expiresAtUtc)
                            font.pixelSize: P.Style.fontSm; color: P.Style.faint }
                    }
                    // 右位：可用=去使用按钮，其余=状态签
                    Item {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 96; height: 40
                        P.ActionButton {
                            objectName: "couponGoUseButton"
                            anchors.centerIn: parent
                            variant: "primary"; text: "去使用"
                            visible: modelData.status === "available"
                            onClicked: {
                                // TODO(contract): couponService.redeem(id) → 跳预约确认携带券 id。
                                try {
                                    if (typeof couponService !== "undefined" && couponService)
                                        couponService.redeem(modelData.id)
                                } catch (e) { /* 桥缺位：仅提示 */ }
                                // 核销桥未就绪时只提示、不改本页券状态与计数（券状态归服务端裁决）；
                                // 文案按 demo/真券区分口径，同"演示数据不冒充"原则。
                                if (App) App.showToast(page.demo ? "券核销桥未就绪（演示数据）"
                                                                 : "去使用桥未就绪", "warning")
                            }
                        }
                        P.StatusTag {
                            anchors.centerIn: parent
                            visible: modelData.status !== "available"
                            tone: modelData.status === "used" ? "success" : "neutral"
                            text: modelData.status === "used" ? "已使用" : "已过期"
                        }
                    }
                }
            }
        }

        // ---- 空态（与列表互斥；三 tab 共用一个面板，文案按页签索引） ----
        P.NoticePanel {
            objectName: "couponEmptyNotice"
            width: parent.width
            height: 160
            visible: page.rowsFor(page.tab).length === 0
            glyph: "ticket"
            title: ["暂无可用优惠券", "暂无已使用的券", "暂无已过期的券"][page.tab]
            description: "充值回馈与平台活动发的券会出现在这里；结算页可勾选抵扣。"
            actionText: ""
        }
    }
}
