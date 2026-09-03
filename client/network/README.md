# Client Network

放置 TCP 连接、重连、帧收发、请求超时和 `requestId` 关联实现。该模块不得包含页面或
SQLite 代码。当前 `ClientConnection` 已实现最小登录所需的异步连接、帧收发、10 秒超时
和 `requestId` 响应匹配；后续重连策略继续在本模块扩展。
