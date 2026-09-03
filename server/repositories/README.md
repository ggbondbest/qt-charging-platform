# Server Repositories

放置 Repository 接口和 SQLite 实现。该模块只负责查询与持久化，不弹窗、不构造协议
响应；Schema 变更走独立契约 PR。当前 `UserRepository` 已实现按手机号查询和并发安全的
首次创建；其他业务 Repository 仍待实现。
