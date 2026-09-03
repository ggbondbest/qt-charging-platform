# Server Database Runtime

放置数据库路径、连接生命周期、PRAGMA、Schema 初始化和迁移执行器。SQL 定义仍保留在
仓库顶层 `database/`。当前 `DatabaseConnection` 已实现 QSQLITE 连接生命周期，并从 Qt
资源逐条执行 Schema 和可选 Seed；后续版本迁移继续在本模块扩展。
