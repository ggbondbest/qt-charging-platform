# Station Services（成员 2）

用户端页面用例的服务层。当前提供：

- `AuthService`：手机号校验、`USER_LOGIN` 请求以及登录成功/失败信号。
- `StationQueryService`（任务 #7/#12）：站点检索与站点详情双通道——服务端
  `GET_STATIONS` / `GET_CHARGERS` 未就绪前默认走模拟数据通道（带延迟，驱动
  页面加载状态；详情桩数据覆盖空闲/占用/故障/离线，并含无桩站与离线站演示
  用例）；注入 `ClientConnection` 并 `setLiveMode(true)` 后无缝切换到真实
  请求，两条通道输出同一 `querySucceeded(StationList)` /
  `detailSucceeded(StationDetail)` 信号，页面 UI 逻辑不变。路由携带非法站点
  ID 时详情通道回友好 `detailFailed`。排序/电价筛选是页面对结果的本地投影，
  不经过本服务重复请求。任务 #17 增加 `setMockChargerReserved(chargerId)`：
  仅作用于模拟通道，预约成功后把对应桩覆盖为“已预约”并重算空位数，
  供详情页刷新演示；真实通道以服务端桩状态为准，不使用该覆盖。
