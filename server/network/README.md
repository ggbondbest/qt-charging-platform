# Server Network

放置 listener、ClientSession、帧 decoder 和 RequestDispatcher。网络对象只做非阻塞收发、
协议校验与路由；业务状态和事务交给 Server Services。`TcpServer` 是 listener 的统一
类名入口，底层沿用 `ChargingServer`。

`ClientSession` 从成功的 `USER_LOGIN` 绑定 user ID；`RequestDispatcher` 已路由登录、预约、
取消、开始、实时状态、停止和支付。业务请求只信任 Session 身份，ID 必须是正十进制
JSON 字符串。未登录、非本人资源、非法 ID、未知动作和内部异常都返回稳定错误码。
