# Server Repositories

放置 Repository 接口和 SQLite 实现。该模块只负责查询与持久化，不弹窗、不构造协议
响应；Schema 变更走独立契约 PR。当前 target 是无源码 `INTERFACE` 边界；首次加入
`.cpp` 时，只在本目录将它改为 `STATIC` 并登记源文件。
