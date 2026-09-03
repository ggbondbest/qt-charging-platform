# Server Services

放置登录、预约、充电、计费、订单和后台管理用例。Service 负责校验、状态机与事务边界，
通过 Repository 持久化，不引用 Widgets 或直接处理 Socket 字节流。当前 `UserService`
已实现手机号登录/自动注册和冻结用户拦截；预约、充电、计费等 Service 仍待实现。
