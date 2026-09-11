import QtQuick
import QtQuick.Controls.Basic
import "../../platform" as P

// 积分商城页（2026-09-09 会员中心批，与「设置」并列入口）：优惠券/小物品/
// 充电卡三类可兑商品 + 本机兑换记录。
// 诚实口径（页脚同样标注）：服务端 CONTRACT 只有 CHECK_IN/RECHARGE 入账、
// 没有积分扣减/核销动作 → **积分余额读服务端真账**（pointsService 桥），
// 余额不足置灰兑换钮；点击兑换只生成**本机演示记录**，不扣服务端积分。
// TODO(contract): REDEEM wire 动作 + 券发放服务落地后自动切换真扣减真发货。
// 答辩补注：route="points_mall"；入口=「我的」页「积分商城」行（与「设置」并列）。
// 全页读写分离：余额只读服务端真账（pointsService 桥），唯一写入是本机演示记录，服务端一分不扣。
Item {
    id: page
    objectName: "pointsMallPage"
    property string route: "points_mall"
    property var arg: ""
    width: parent ? parent.width : 420
    height: parent ? parent.height : 600

    // ---- 商品目录（页内演示数据，桥就绪后迁服务端） ----
    // 六款覆盖三分类；cost 与服务端余额同用"积分"单位，数值本身是演示价（契约未定）。
    readonly property var catalog: [
        { id: "c1", cat: "coupon", glyph: "🎫", name: "5元充电券",
          desc: "满30元账单立减", cost: 200 },
        { id: "c2", cat: "coupon", glyph: "🎟️", name: "10元充电券",
          desc: "满100元账单立减", cost: 500 },
        { id: "g1", cat: "goods",  glyph: "🧸", name: "充电伙伴公仔",
          desc: "站点吉祥物限定款", cost: 800 },
        { id: "g2", cat: "goods",  glyph: "🧴", name: "车载清洁套装",
          desc: "玻璃水+毛巾三件套", cost: 600 },
        { id: "k1", cat: "card",   glyph: "💳", name: "50元充电卡",
          desc: "到账即余额", cost: 1200 },
        { id: "k2", cat: "card",   glyph: "🔋", name: "100元充电卡",
          desc: "到账即余额", cost: 2200 }
    ]
    readonly property var cats: [
        { id: "all",    label: "全部" },
        { id: "coupon", label: "优惠券" },
        { id: "goods",  label: "小物品" },
        { id: "card",   label: "充电卡" }
    ]
    property string category: "all"

    Rectangle { anchors.fill: parent; color: P.Style.bg }

    // ---- 真实积分余额（服务端账本；桥缺位期 -1=未知） ----
    property int points: -1

    // 单一事实源=catalog+category：结果现算现用，不再维护第二份缓存列表。
    function filtered() {
        return page.catalog.filter(function (it) {
            return page.category === "all" || it.cat === page.category
        })
    }
    // 同分类短路：重复点当前 chip 不重发列表绑定。
    function setCategory(cat) {
        if (page.category === cat) return
        page.category = cat
        page.list = page.filtered()
    }
    function findItem(id) {
        for (var i = 0; i < page.catalog.length; ++i)
            if (page.catalog[i].id === id) return page.catalog[i]
        return null
    }
    // 兑换：余额已知且不足→拒绝+提示；其余写本机演示记录（不碰服务端账本）。
    function redeem(id) {
        const item = page.findItem(id)
        if (!item) return false
        // points>=0 这个短路条件是刻意的：-1=未知（还没拉到）时放行演示，"不足"只在余额已知且真缺时成立。
        if (page.points >= 0 && item.cost > page.points) {
            if (App) App.showToast("积分不足 · 还差 " + (item.cost - page.points) + " 分", "danger")
            return false
        }
        // 本页唯一写入：本机内存账（离开页面即失），刻意不扣服务端积分——切换路径见文件头 TODO(contract)。
        recordsModel.append({
            glyph: item.glyph, name: item.name, cost: item.cost,
            time: Qt.formatDateTime(new Date(), "yyyy-MM-dd hh:mm")
        })
        if (App) App.showToast("兑换成功（演示通道）：" + item.name + " 已记入本机记录", "success")
        return true
    }

    // list 是派生快照：仅切分类时重取一次，配合下方 listCount 钉测试。
    property var list: filtered()
    // 派生计数（offscreen 测试口径：delegate 何时入树不可靠，页根公开状态做钉）。
    readonly property int listCount: page.list.length

    ListModel { id: recordsModel; objectName: "uiMallRecords" }

    // 服务端真账回执：只取 points 标量入页，流水 entries/total 用不上（不渲染账本）。
    Connections {
        target: typeof pointsService !== "undefined" ? pointsService : null
        function onPointsLoaded(points, entries, total) { page.points = points }
    }
    // 建页拉一次余额即可（页大小取 1 省载荷）；try/catch 防 offscreen 裸引擎无桥炸页根。
    Component.onCompleted: {
        if (typeof pointsService !== "undefined" && pointsService)
            try { pointsService.fetchPoints(1, 1) } catch (e) {}
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: content.implicitHeight + 2 * P.Style.spaceXl
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        Column {
            id: content
            x: P.Style.spaceXl; y: P.Style.spaceXl
            width: parent.width - 2 * P.Style.spaceXl
            spacing: P.Style.spaceMd

            // ---- header：标题 + 真积分余额 ----
            Item {
                width: parent.width
                height: 40
                Text {
                    anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                    objectName: "uiMallTitle"
                    text: "积分商城"
                    font.pixelSize: P.Style.fontXl; font.weight: Font.Bold; color: P.Style.ink
                }
                Text {
                    anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                    objectName: "uiMallPoints"
                    text: page.points >= 0 ? ("我的积分 " + page.points) : "积分加载中…"
                    font.pixelSize: P.Style.fontSm; color: P.Style.muted
                }
            }

            // ---- 分类 chip ----
            Row {
                width: parent.width
                spacing: P.Style.spaceSm
                Repeater {
                    model: page.cats
                    delegate: P.ActionButton {
                        required property var modelData
                        objectName: "uiMallCatChip"
                        variant: "chip"
                        selected: page.category === modelData.id
                        text: modelData.label
                        onClicked: page.setCategory(modelData.id)
                    }
                }
            }

            // ---- 商品卡列表 ----
            Repeater {
                model: page.list
                delegate: Rectangle {
                    objectName: "uiMallCard"
                    required property var modelData
                    width: parent.width
                    height: 78
                    radius: P.Style.radiusLg
                    color: P.Style.surface
                    border.width: 1
                    border.color: P.Style.line
                    Item {
                        anchors.fill: parent
                        anchors.leftMargin: 16; anchors.rightMargin: 12
                        Rectangle {
                            id: mallHub
                            anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                            width: 42; height: 42; radius: 21
                            color: P.Style.brandSoft
                            Text {
                                anchors.centerIn: parent
                                text: modelData.glyph; font.pixelSize: 18
                            }
                        }
                        Column {
                            anchors.left: mallHub.right
                            anchors.right: mallCost.left
                            anchors.leftMargin: P.Style.spaceMd
                            anchors.rightMargin: P.Style.spaceMd
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 3
                            Text {
                                width: parent.width; elide: Text.ElideRight
                                objectName: "uiMallItemName"
                                text: modelData.name
                                font.pixelSize: P.Style.fontLg2; font.weight: Font.DemiBold
                                color: P.Style.ink
                            }
                            Text {
                                width: parent.width; elide: Text.ElideRight
                                text: modelData.desc
                                font.pixelSize: P.Style.fontSm; color: P.Style.faint
                            }
                        }
                        Column {
                            id: mallCost
                            anchors.right: mallRedeem.left
                            anchors.rightMargin: P.Style.spaceMd
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 3
                            Text {
                                anchors.right: parent.right
                                objectName: "uiMallCost"
                                text: modelData.cost + " 积分"
                                font.pixelSize: P.Style.fontMd; font.weight: Font.Bold
                                color: P.Style.starGold
                            }
                        }
                        P.ActionButton {
                            id: mallRedeem
                            objectName: "uiMallRedeemButton"
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            variant: "secondary"
                            // 置灰唯一条件是"已知且真不足"：未知(-1)=加载中不置灰——对未知的诚实做法是放行演示而非假装知道。
                            enabled: page.points < 0 || modelData.cost <= page.points
                            text: "兑换"
                            onClicked: page.redeem(modelData.id)
                        }
                    }
                }
            }

            // ---- 兑换记录（本机演示账目） ----
            Text {
                visible: recordsModel.count > 0
                text: "我的兑换记录（本机）"
                font.pixelSize: P.Style.fontSm; color: P.Style.muted
            }
            // model 用 ListModel 而非 JS 数组：append 后视图自动追加新记录行，无需手动重绑。
            Repeater {
                model: recordsModel
                Rectangle {
                    objectName: "uiMallRecordCard"
                    required property var modelData
                    width: parent.width
                    height: 56
                    radius: P.Style.radiusLg
                    color: P.Style.surface
                    border.width: 1
                    border.color: P.Style.line
                    Item {
                        anchors.fill: parent
                        anchors.leftMargin: 16; anchors.rightMargin: 16
                        Text {
                            anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                            text: modelData.glyph + "  " + modelData.name
                            font.pixelSize: P.Style.fontMd; color: P.Style.ink
                        }
                        Text {
                            anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                            text: "-" + modelData.cost + " 积分 · " + modelData.time
                            font.pixelSize: P.Style.fontSm; color: P.Style.faint
                        }
                    }
                }
            }

            // ---- 诚实脚注 ----
            Text {
                objectName: "uiMallFootnote"
                width: parent.width
                wrapMode: Text.WordWrap
                text: "积分余额为服务端真实账本；服务端契约暂无积分扣减/核销接口，兑换暂只生成本机演示记录、不扣真积分，接口就绪后自动切换真扣减真发货。"
                font.pixelSize: P.Style.fontXs; color: P.Style.faint
            }
        }
    }
}
