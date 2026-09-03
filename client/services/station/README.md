# Station Services（成员 2）

用户端页面用例的服务层。当前提供：

- `AuthService`：手机号校验、`USER_LOGIN` 请求以及登录成功/失败信号。
- `StationQueryService`（任务 #7）：站点检索。双通道设计——服务端
  `GET_STATIONS` 未就绪前默认走模拟数据通道（带延迟，驱动页面加载状态）；
  注入 `ClientConnection` 并 `setLiveMode(true)` 后无缝切换到真实请求，
  两条通道输出同一 `querySucceeded(StationList)` 信号，页面 UI 逻辑不变。
  排序/电价筛选是页面对结果的本地投影，不经过本服务重复请求。
