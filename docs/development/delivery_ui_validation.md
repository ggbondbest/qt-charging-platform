# 地图与双端界面交付验收

本文件包含操作步骤及下方已执行记录；未执行项目仍明确标为待验证。

## 本地修复分支

分支为 `fix/delivery-map-ui`，基于 `develop` 的 `e0172a1`。
PR48 的合并历史已包含 PR49，因此不是直接切换到 PR48：本分支撤回 PR49
的首页/导航交互和内置地图 Key 配置，再实现单一的手动地址导航及真实首页地图。
保留 PR48 的统计、积分、优惠券、评价、扫码等业务，及后续连续定位恢复、
数据库并发修复。没有重置远端 `develop`，没有删除已有业务数据库。

首页地图标记的预约入口先进入电站详情选桩，再走服务端准入、预约确认；
不绕过空闲状态检查，不使用固定假订单。导航页面退出后释放自己的路线，
旧页面不得抢占新目标或取消新页面的请求。

## 构建与页面冒烟

验收基线为 Ubuntu 22.04、Qt Framework **6.2.4**、C++17。Qt Creator 的版本
不能代替 Framework 版本。完整脚本保留严格版本检查：

```bash
bash scripts/verify_delivery.sh
```

仅做页面加载检查时，沿用原有两个参数；默认每页运行一次，不自动展开尺寸与主题矩阵。
脚本覆盖 23 个页面，包括积分、评价、统计、优惠券、扫码入口：

```bash
bash scripts/verify_qml_routes.sh ./build-delivery/client/charging-client ./runtime/ui-light
CHARGING_SMOKE_SIZE=360x740 CHARGING_SMOKE_THEME=light \
  bash scripts/verify_qml_routes.sh ./build-delivery/client/charging-client ./runtime/ui-narrow
CHARGING_SMOKE_SIZE=420x860 CHARGING_SMOKE_THEME=dark \
  bash scripts/verify_qml_routes.sh ./build-delivery/client/charging-client ./runtime/ui-dark
```

页面脚本明确使用 `CHARGING_CHANNEL=mock`，并清空地图 Key、SK 和 JS Key 环境变量。
截图模式将 QSettings 隔离到截图目录下的 `config/`，亮/暗色检查不会互相污染
或修改正式运行时的界面偏好。
它不使用真实地图服务，不证明服务器、SQLite 或实际道路底图已经联通。截图存在且
页面无 QML 错误，也不证明按钮可点击或没有视觉重叠，需要逐图与实机检查。

## 界面检查

客户端至少检查 420×860 和 360×740 两种窗口尺寸，并分别验证亮色、暗色：

- 搜索框、地址框、下拉框、弹窗有一致的前景/背景颜色，聚焦、禁用和选择文字可读。
- 首页地图、筛选、电价、站点名称、状态、收藏、预约与导航按钮不重叠，不被卡片裁切。
- 点击收藏和导航不触发整张卡片的跳页；页面滚动、地图拖动和按钮点击可分别使用。
- 路线地图与文字步骤各有自己的布局空间；长地址、错误消息不会盖住返回或重新规划。
- 积分、评价、优惠券、统计、扫码页面可打开、返回与刷新，空数据/加载失败有明确提示。
- 窗口缩小时可滚动到底部；设置中增大字号后，重点检查站点卡片、订单和弹窗。

管理端至少在 1280×800 下登录并检查运营概览、电站、电桩、用户、订单、充值和日志：

- 筛选框、表头、按钮、选中行、错误消息及图表文字可读，无黑色输入框混入亮色页面。
- 新增/编辑弹窗字段与确认/取消按钮完整显示，长名称和错误信息不挤出窗口。
- 表格可滚动、分页和刷新，动作进行中有反馈；管理页不能用演示成功提示代替实际返回。

## 手动起点与真实地图

启动前只在客户端运行环境中配置使用者自己的 `TENCENT_MAP_API_KEY`；启用签名校验
时再设置对应 `TENCENT_MAP_SECRET_KEY`。底图独立 Key 使用 `TENCENT_MAP_JS_KEY`。
不要在代码、截图、测试报告或完整请求 URL 中保存真实凭证。

1. 未配置 Key 时，检查明确的配置提示；不可把网格、固定坐标或模拟路线显示成真实成功。
2. 配置 Key 后，首页应加载真实道路底图。以大连作为初始浏览中心只是地图视角，不能
   将该中心当作用户已经定位成功的位置，也不能据此生成“距我”或实际导航起点。
3. 首页站点标记必须对应服务器返回的站点 ID 和坐标；演示库的示范电站不等于现实中
   实际运营的电站。地图展示与站点列表必须指向同一条站点记录。
4. 点击地图中的站点，验证名称、空闲桩、电价与列表一致；预约应进入选择空闲桩及确认
   流程，导航以所选电站为终点。暂停站点、无空闲桩、无有效坐标应有明确限制。
5. 输入起点 A 并定位，不进入导航，换起点 B，再换回 A；成功坐标、标签与距离保持一致。
   成功缓存命中不代表第二次真的发送过 HTTP；应另用新地址验证请求仍可恢复。
6. 请求未结束时再次点击/按回车、修改地址、返回页面；旧响应不可覆盖新起点，失败后
   可再次定位。仅记录安全的 HTTP/业务状态码，不输出 Key、SK 或签名。
7. 打开导航，验证驾驶和步行两种真实路线；起点是用户最后一次成功定位的地址，终点
   是所选电站。首页距离为直线距离，路线距离/时间来自地图服务，二者无需相等。
8. 网络断开、地图拒绝请求、底图加载失败时显示具体状态，不静默换为假地图或假路线。

## 最小真实业务闭环

使用独立演示数据库文件；不得通过删除已有业务库来获得“干净”测试。客户端运行环境
不要设置 `CHARGING_CHANNEL=mock`。启动方法见 `delivery_acceptance.md`。

1. 管理员登录；新手机号登录客户端，核对新账户余额为零。
2. 客户端充值、修改昵称/头像；退出再登录，管理端查询同一用户，核对持久化结果。
3. 通过首页地图选择服务器中的电站，查询空闲桩、确认预约，再开始充电。
4. 充电中退出重登，应恢复原订单；再次预约不能绕过未完成订单检查。
5. 停止充电进入待支付，支付完成后核对客户端余额、管理端订单金额及电桩空闲状态。
   重复支付不可二次扣款；预约冲突、余额不足必须显示服务器返回的失败。
6. 管理端冻结无活动订单用户，检查客户端拒绝继续操作；断开服务端应退出旧会话，
   重新连接不能继续显示上个用户的私有缓存。

自动化优先检查 `qml_tcp_delivery`、`user_api_integration`、`charging_workflow`、
`charging_tcp_integration`、`database_maintenance` 和管理接口/页面测试；先使用
`ctest --test-dir build-delivery -N` 核对实际测试名称。`qml_tcp_delivery` 已包含
真实 TCP、QML 充值/停止/支付以及管理端查询同一数据库的链路，应避免回退时删除。

## 真实性与交付边界

- 收费设备目前是实训模拟：依据服务端时间、额定功率计费，不声称连接了真实电桩。
- 收藏与界面偏好使用本地配置；车辆信息目前仅内存，不能宣称这些都跨设备同步到 SQLite。
- 扫码页的手工桩码/选择站点入口不是摄像头识别二维码；须在演示中明确说明。
- 自动化假地图服务验证请求状态、错误分类和回调，不验证真实 Key 权限、网络和地图配额。
- 旧版数据库备份恢复必须使用副本单独验收；新建库通过不能替代历史备份恢复检查。
- 本地较新 Qt 构建通过不能替代 Qt 6.2.4 编译验证；未做的人工地图验收不得标记为通过。

## 本地验收记录（2026-09-09）

| 检查 | 版本/命令/证据 | 实际结果 |
| --- | --- | --- |
| 代码与 Qt Framework 版本 | 本分支源码；Qt 6.12.0；提交号见 `git log -1` | 本地完整 Debug 构建通过 |
| CTest | `ctest --test-dir build --output-on-failure --parallel 4` | 50/50 测试程序通过 |
| Qt 6.2.4 严格版本构建 | Ubuntu 中运行 `scripts/verify_delivery.sh` | 待验收环境验证，不能以较新 Qt 替代 |
| 23 页默认冒烟 | `build/ui-light/`，420×860/light | 23/23 通过 |
| 小窗口、暗色冒烟 | `build/ui-narrow/`，360×740/light；`build/ui-dark/`，420×860/dark | 各 23/23 通过；三组共 69 次 |
| 客户端视觉/点击 | `build/ui-interactions/`；`qml_platform_controls`、`qml_station_interactions`、`qml_navigation_page` | 卡片价格/按钮、输入、下拉、预约准入、滚动与导航页状态通过；不是所有页面大字号都已人工验收 |
| 管理端页面及弹窗检查 | `admin_ui_presentation` 及下方管理端记录 | 已通过 |
| WebEngine 文档加载/站点点击 | `qml_map_views`：真实浏览器加载离线 HTML、真实鼠标点击站点 ID，禁止外站跳转 | 已通过；不依赖实际地图 Key |
| 连续定位及驾驶/步行请求 | `map_geo_service`、`map_geocoding_recovery`、`qml_map_bridge`、`qml_navigation_page` | 假 HTTP 服务及页面状态回归通过，保留缓存/合并/错误恢复 |
| 真实腾讯底图/道路与 Key 授权 | 使用自己的 Key 依照上方步骤人工验证 | 待用户环境联网验证，未使用或展示真实凭证 |
| TCP 预约→充电→待支付→支付及管理端核对 | `qml_tcp_delivery`、`user_api_integration`、`charging_workflow`、`charging_tcp_integration` | 真实本机 TCP 与独立 SQLite 测试通过 |
| 旧备份副本恢复（需要使用旧备份时） | `database_maintenance` 自动回归通过；已有旧库副本另行检查 | 用户旧备份未人工验收 |

### 管理端已执行记录（2026-09-09）

以下结果来自当前交付分支工作树；提交 SHA 应在合并前补录。使用 Qt Framework
6.12.0、`QT_QPA_PLATFORM=offscreen`，主动将应用调色板设置为深色后运行，
不等同于已通过 Qt 6.2.4 或验收虚拟机验证。

- `admin_ui_presentation`、`admin_login`、`admin_management_pages`、
  `delivery_admin_pages` 四组测试全部通过（合计约 20 秒）。
- 真实管理员登录与独立 SQLite 演示库：运营概览、电桩、电站、用户、订单、
  充值和日志页面已生成截图；未使用地图 Key 或真实用户数据。
- 已目视检查 1600×990 管理页及 1024×720 登录、电站页面和滚动后的用户详情。
  小窗口保留水平/垂直滚动，不宣称完整宽表能在 1024 像素内同时显示。
- 新增/编辑电站弹窗已检查：真实服务模式仅显示会提交的字段；输入框与确认/取消
  按钮不超出 1024×720 窗口。演示样例下新增弹窗约 520×565，编辑弹窗约 520×424。
- 深色调色板下输入文字、表格交替行、图表边缘背景有自动断言；电桩所属电站列
  不再挤成逐字换行。冻结按钮在紧凑窗口滚动到详情后可到达（截图测试未执行冻结）。

复验命令（先构建对应测试）：

```bash
QT_QPA_PLATFORM=offscreen CHARGING_ADMIN_UI_SCREENSHOTS=./runtime/admin-ui \
  ctest --test-dir build-admin \
  -R '^(admin_ui_presentation|admin_login|admin_management_pages|delivery_admin_pages)$' \
  --output-on-failure
```

本次本地截图位于 `build-admin/ui-screenshots/`，包括 `admin-login-1024.png`、
`admin-dashboard-1600.png`、`admin-stations-1600.png`、
`admin-station-create-dialog.png`、`admin-station-edit-dialog.png` 和
`admin-user-detail-1024.png`。这些是生成的检查证据，不作为运行依赖提交。

### 站点交互已执行记录（2026-09-09）

`qml_station_interactions` 已适配本次首页和手动定位方式；在 Qt 6.12.0、offscreen
与 software Quick 后端下通过全部用例（QtTest 报告 11 passed，含初始化/清理）。
验证了真实鼠标点击收藏星、卡片、铃铛、个人页消息入口；地图站点弹窗拒绝未知 ID，
已知 ID 进入同一站点详情；420/360 宽度下电价及预约/导航按钮位于卡片内部。

预约正反例使用 App 自身的 MockRequestTransport：清空未完成订单后，零车辆可进入
预约确认；存在模拟充电订单时由实际订单查询检查转向充电页，而不是直接注入 UI 布尔值。
地址定位用例清空所有地图凭证，验证三次地址提交在缺 Key 失败后均能恢复可操作状态；
它不证明腾讯外网连续请求成功，也不替代真实地图验收。

卡片截图已目视检查，位于 `build/ui-interactions/station-card-420.png` 和
`build/ui-interactions/station-card-360.png`。可用环境变量
`CHARGING_INTERACTION_SCREENSHOTS` 为该测试指定截图目录；测试使用隔离偏好目录，
不修改实际用户的收藏或界面设置。
