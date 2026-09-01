# Server Database Runtime

放置数据库路径、连接生命周期、PRAGMA、Schema 初始化和迁移执行器。SQL 定义仍保留在
仓库顶层 `database/`，其资源 target 由最终 Server 可执行程序链接；运行时代码只登记到
本目录 CMake target。当前 target 是无源码 `INTERFACE` 边界；首次加入 `.cpp` 时，只在
本目录将它改为 `STATIC` 并登记源文件。
