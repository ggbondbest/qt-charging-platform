# Client Network

放置 TCP 连接、重连、帧收发、请求超时和 `requestId` 关联实现。该模块不得包含页面或
SQLite 代码。

`NetworkManager` 是组员使用的统一类名入口，底层沿用已通过登录闭环验证的
`ClientConnection`。请求用 UUID `requestId` 匹配响应，未连接时提交的多个请求会按 FIFO
发送。超时、断线、Socket 错误或响应协议错误会结束相关待处理请求，同一请求只通知一次失败。
后续的指数退避自动重连策略继续在本模块扩展。
