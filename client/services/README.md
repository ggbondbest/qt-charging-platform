# Client Services

放置面向页面的异步用例接口和状态协调。Service 可以依赖 Client Network 与 Common，
不能直接访问 `QTcpSocket` 细节或 SQLite。顶层 target 只聚合子模块，成员 2 修改
`station/`，成员 3 修改 `profile_charging/`，不在本文件追加功能源文件。
