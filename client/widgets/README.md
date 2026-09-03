# Client Widgets

放置多个用户端页面复用的控件。全局设计 token 或 QSS 变更需单独评审；页面专属控件
留在对应页面附近，避免把本目录变成无边界的组件集合。

当前为 `STATIC` target，已提供第一批共享组件（成员3 本地候选实现，未提交）：
`Card`、`ActionButton`、`StatusTag`、`Toast`、`LoadingOverlay`、`NoticePanel`，
配套 token 样式位于 `resources/qss/client_platform.qss`（经 `client_platform.qrc`
登记为 `:/qss/client_platform.qss`）。合入前全局 QSS 变更需组长评审。
