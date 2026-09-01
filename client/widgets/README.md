# Client Widgets

放置多个用户端页面复用的控件。全局设计 token 或 QSS 变更需单独评审；页面专属控件
留在对应页面附近，避免把本目录变成无边界的组件集合。当前 target 是无源码
`INTERFACE` 边界；首次加入 `.cpp` 时，只在本目录将它改为 `STATIC` 并登记源文件。
