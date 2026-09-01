# Server Services

放置登录、预约、充电、计费、订单和后台管理用例。Service 负责校验、状态机与事务边界，
通过 Repository 持久化，不引用 Widgets 或直接处理 Socket 字节流。当前 target 是无源码
`INTERFACE` 边界；首次加入 `.cpp` 时，只在本目录将它改为 `STATIC` 并登记源文件。
