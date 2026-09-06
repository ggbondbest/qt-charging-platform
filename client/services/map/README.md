# 腾讯地图 WebService 服务（成员 2）

`MapGeoService`：C++ 层封装腾讯位置服务 WebService API，三个接口：

| 方法 | 接口 | 用途 | 关键返回 |
| --- | --- | --- | --- |
| `requestDistanceMatrix(destinations)` | `ws/distance/v1/matrix/`（mode=driving） | 用户位置 → 站点的**行驶距离 + 预估行驶时长**，供预约"系统推荐时段"使用 | `rows[0].elements[].distance`（米）、`duration`（**秒**） |
| `requestDrivingRoute(from, to)` | `ws/direction/v1/driving/` | 导航页**驾车路线**（分段步骤文案 + 坐标折线） | `routes[0].distance`（米）、`duration`（**分钟**）、`steps[].instruction`、`polyline`（`QVector<LatLng>`，见下文解码口径） |
| `requestReverseGeocode(location)`（可选） | `ws/geocoder/v1/` | 坐标 → 地址文本（导航页"前往"行补真实地址） | `result.address` |

> 路线接口真实响应结构为 `result.routes[0]`（早期文档写作 `result.mode`）；
> 解析器优先 `routes[0]`、回退兼容 `mode`，两种口径都有单测锁定。

## Key 申请与配置（一次性操作）

1. 打开[腾讯位置服务官网](https://lbs.qq.com/)，QQ 账号注册/登录，完成开发者**实名认证**（个人认证即可）。
2. 控制台 → 应用管理 → 我的应用 → **创建应用**（如 `qt-charging-platform`）。
3. 应用内**添加 Key**，勾选 **WebServiceAPI**（距离计算、路线规划、地理编码包含在内，无需单独开通接口权限）：
   - 域名白名单选"不设置"（桌面客户端无域名；也可配 IP 白名单）；
   - **签名校验**先选"不设置"，Key 直接可用。若之后开启签名校验，把 SK 配到环境变量 `TENCENT_MAP_SECRET_KEY`，本服务自动按官方规则附带 `sig` 参数。
4. 本机注入环境变量（**切勿写入仓库/代码/群聊**）：
   ```bash
   export TENCENT_MAP_API_KEY=你的key
   ```
   （兼容旧名 `CHARGING_TENCENT_MAP_KEY`，两者都在时新名优先。）
   图形环境启动 Qt Creator 时在 项目 → 运行 → 环境变量 里加同名项。
5. 验证：启动客户端进入预约确认页，"✨ 推荐"按钮分钟数变为接口口径（标注"真实路况"）；导航页 caption 依次显示"正在加载真实导航路线…"→"真实导航路线 · 腾讯地图"，"前往"行追加逆地理地址。控制台配额页可核对调用量。

## 异常兜底口径（任务书第 3 条）

密钥无效 / 网络不通 / 超时 / 限流 → `*Failed(requestId, MapError, 中文文案)`。
**兜底策略在消费方页面**（本服务不含预约业务语义）：

- `reservation_confirm_page`：推荐时段保持模拟估算（`ReservationService::recommendSlot` 口径不变），Toast 提示"地图服务暂不可用（原因）"；
- `navigation_page`：保持 `buildMockSteps` 模拟路线，caption 追加"接口异常：原因"；逆地理失败静默，"前往"行回落站名口径。

## 路线折线（polyline）解码口径

官方格式（[JavaScript GL  polyline 指南](https://lbs.qq.com/javascript_gl/guide-polyline.html)同规则）：
数组为 `[纬度₀, 经度₀, Δ纬₁(微度), Δ经₁, Δ纬₂, Δ经₂, …]`——**前两个元素是首点
绝对度数**，其后成对的是整数**微度增量**，递推规则 `coors[i] = coors[i-2] + coors[i]/1e6`。

实现口径（`map_geo_service.cpp`，3 条单测锁定）：

- 首点若量级 >1000 视为按微度返回的版本，÷1e6 归一（两种版本都能解）；
- 逐点解码后校验落在中国范围（纬 15–55 / 经 73–136），任一点越界即判定
  脏数据，**整条折线置空**（防"飞线"横穿地图的渲染事故）；
- `polyline` 为空 = 消费方回落口径：导航页 `updateRouteMap()` 用起终点
  垂直偏移正弦扰动画模拟折线，地图永不空白。

渲染链路（可视化迭代）：`RouteResult.polyline` → `NavigationPage::realPolyline_`
→ `StationMapPanel::setRoutePoints()`（缓存后复用 `setStations()` 渲染通道）
→ `tencent_map.html` 注入 `%ROUTE_POINTS%` → `qq.maps.Polyline`（平台绿
`#00B578`）+ `fitBounds` 自动包住全程。**空数组 = 首页口径逐字节不变**（只画
站点标记不画线），首页/导航共用同一面板组件。

页面均有 **loading 态**（任务书第 3 条）：确认页推荐按钮追加"（更新中…）"、导航页 caption"正在加载真实导航路线…"，异步信号回填后消失，全程不阻塞、不卡死。

**未配置 key 时不发起任何网络请求**，直接异步回 `NoApiKey`——CI（无 key、无外网）行为与接入前完全一致。

## 安全红线

- Key/SK 仅经环境变量注入；错误文案只透出固定分类（不回显接口 message 原文）；代码路径中不存在任何打印 URL/密钥的日志。

## 口径说明

- 任务书第 4 条"QML 层接收数据渲染 UI"：本仓库客户端为 **Qt Widgets**（无 QML 栈），UI 层即 `client/pages/station` 各页面，直接消费本服务的 Qt 信号。
- 矩阵 `duration` 单位是秒、路线 `duration` 单位是分钟，结构体字段名（`durationSeconds`/`durationMinutes`）已钉死口径。
- 用户位置：演示口径为南山区固定中心（与站点地图面板一致），真实定位就绪后 `setUserLocation()` 注入即可，页面零改动。
