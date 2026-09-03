# Client Widgets

放置多个用户端页面复用的控件。全局设计 token 或 QSS 变更需单独评审；页面专属控件
留在对应页面附近，避免把本目录变成无边界的组件集合。

当前为 `STATIC` target，已提供第一批共享组件（成员3 本地候选实现，未提交）：
`Card`、`ActionButton`、`StatusTag`、`Toast`、`LoadingOverlay`、`NoticePanel`，
配套 token 样式位于 `resources/qss/client_platform.qss`（经 `client_platform.qrc`
登记为 `:/qss/client_platform.qss`）。合入前全局 QSS 变更需组长评审。

成员 2（任务 #2 交付物）新增两个导航公共组件，所有用户端页面复用：
`TopNavBar`（左 Logo+平台名 / 中间站点搜索框 / 右侧登录态动态区：未登录显示
“登录”按钮，已登录显示头像点击跳个人中心）、`BottomTabBar`（固定底部 Tab，
首页注入 找站/订单/充值/我的 四项）。两者样式以对象名限定在组件内部生效，
**不改动全局 QSS 文件**；因 `TopNavBar` 透传 `model::User`，本 target 新增
依赖 `ChargingPlatform::Common`。
