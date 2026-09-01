# Client Network

放置 TCP 连接、重连、帧收发、请求超时和 `requestId` 关联实现。该模块不得包含页面或
SQLite 代码。当前 target 是无源码 `INTERFACE` 边界；首次加入 `.cpp` 时，只在本目录将
它改为 `STATIC` 并登记源文件，不修改根 CMake。
