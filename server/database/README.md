# Server Database Runtime

放置数据库路径、连接生命周期、PRAGMA、Schema 初始化和迁移执行器。SQL 定义仍保留在
仓库顶层 `database/`。当前 `DatabaseConnection` 已实现 QSQLITE 连接生命周期，并从 Qt
资源逐条执行 Schema 和可选 Seed；后续版本迁移继续在本模块扩展。

`applyCityDemoSeed()` 是显式的五市示范目录补齐入口，由 `ServerRuntime` 在
`--demo-seed` 启动时调用；基础 `open(path, true)` 仍保留 3 站 / 7 桩最小夹具。
目录插入幂等、整批事务回滚，不覆盖已有记录。已有库升级和数据真实性边界见
[database/README.md](../../database/README.md)。

`DatabaseMaintenance` 提供数据库运维闭环：对运行中的 SQLite 数据库使用
`VACUUM INTO` 生成一致性备份，备份与恢复前执行完整性、外键和 Schema 版本校验，
恢复时使用原子写入，并要求调用方先关闭目标数据库连接。
