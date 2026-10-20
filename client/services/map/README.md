# 腾讯地图 WebService 服务

`MapGeoService` 是基于 Qt Network 的异步请求服务，兼容 Qt 6.2.4 / C++17。
正式客户端使用 QML：页面经 `MapBridge` 提交手动地址定位与驾驶/步行路线请求，
服务返回真实坐标、路线步骤及折线；服务器站点数据仍通过 TCP 获取。

## 配置：客户端默认读取统一 JSON

将 `config/services.example.json` 复制为 `config/services.local.json`，填写
`tencentMap.webServiceKey`、可选的 `jsKey` 与 `secretKey`。已存在本地文件时直接编辑，
不要覆盖已填写内容。Qt 客户端原生读取地图配置，直接启动或 Qt Creator 运行即可，
无需 Python 启动器；服务端 AI 使用同一文件的 `ai`，两端各自解析所需配置段。
完整字段说明见 [统一配置说明](../../../docs/services_configuration.md)。

客户端从程序目录及其上级寻找 `config/services.local.json`，随后尝试编译时记录的源码路径，
因此不依赖当前工作目录。也可设置 `CHARGING_SERVICES_CONFIG` 指定文件；显式路径错误不回退。
分发时可放在程序或 `.app` 旁的 `config` 目录。配置中的 Key 不编入二进制。

| 环境变量 | 用途 |
| --- | --- |
| `TENCENT_MAP_API_KEY` | 覆盖地址解析、路线等 WebService Key |
| `TENCENT_MAP_SECRET_KEY` | 覆盖启用签名校验时使用的 SK |
| `TENCENT_MAP_JS_KEY` | 覆盖地图展示用 JavaScript Key |

非空现代环境变量优先，其次兼容旧名 `CHARGING_TENCENT_MAP_KEY` /
`CHARGING_TENCENT_MAP_SECRET`，最后使用文件字段。JavaScript Key 先读专用环境变量，
再读文件 `jsKey`，最后复用最终 WebService Key；仅配置 JS Key 不会启用 WebService。
JS Key 不参与 WebService 签名。

修改配置后完全停止并重新运行客户端。所用 Key 需具备对应 WebService / JavaScript 产品权限；
真实授权、网络及额度需在使用者环境检查。不要提交真实凭证或打印含 Key/SK/sig 的 URL。
未配置时明确返回 `NoApiKey`。不再读取旧 `map_services.json`。

`apiKeyFromConfigFile(path)` 读取统一 JSON 的 WebService Key，`resolveApiKey(path)`
同时应用环境覆盖。文件不能更改请求端点：`baseUrlFromConfigFile()` 返回空，
`resolveBaseUrl()` 固定返回 `https://apis.map.qq.com/ws`；仅测试接缝
`setEndpointBaseForTesting()` 可切换到本机假服务。

## 请求与响应

| 方法 | 用途 |
| --- | --- |
| `requestForwardGeocode(address)` | 手动地址解析为经纬度 |
| `requestDrivingRoute(from, to)` / `requestWalkingRoute(from, to)` | 驾驶/步行路线 |
| `requestReverseGeocode(location)` | 经纬度解析为地址 |
| `requestDistanceMatrix(destinations)` | 从显式设置的起点查询距离矩阵 |

每次调用返回独立 `requestId`，结果通过成功/失败信号异步传回。页面应丢弃过期 ID 的响应。
路线结构含距离（米）、预估时长（分钟）、步骤和解码后的经纬度折线；距离矩阵时长单位
为秒。正式页面必须以用户最后一次成功定位为起点，不能用地图默认浏览中心代替用户位置。

旧 QML 标量方法（如 `requestAddressGeocode`）、IP 定位、静态图和外部导航 URL 方法
为兼容既有调用/测试保留，不代表正式首页会自动 IP 定位或切换到旧导航流程。
默认地图中心、示范站点和真实用户定位是不同概念，不能混作接口成功结果。

## 重复请求与异常

- 两个地址解析入口共享在途请求合并与成功缓存：同一实例、同一地址只发送一次请求，
  每个调用方仍收到自己的 ID 对应结果。缓存最多 32 条、5 分钟，仅保留经过校验的成功坐标。
- 失败不缓存，成功/失败/超时均释放在途状态，之后允许再次定位。
- 地址请求遇到短时限流（业务 status 120 / HTTP 429）最多延迟重试一次；遵守代码中的
  `Retry-After` 上限。日额度状态 121、鉴权失败及未知 HTTP 403 不自动重试。
- 访问拒绝、鉴权失败、限流、日额度、网络、超时分别报告。错误只附安全数字状态码，
  不透传可能含敏感信息的服务端 `message` 或请求 URL。
- 未配置 Key 时不发送 HTTP，异步返回配置错误。正式页面不使用模拟定位、模拟路线
  或绿色网格冒充真实地图成功；路线缺少可用折线时应报告无法显示路线。

## 验证

```bash
ctest --test-dir build-delivery --output-on-failure \
  -R '^(map_service_config|map_geo_service|qml_map_bridge|map_geocoding_recovery|station_map_panel)$'
```

这些测试只使用测试专用占位 Key 和进程内假 HTTP 服务，验证签名、解析、错误分类、
重复定位、JSON 自动发现与环境优先级，不证明使用者的真实地图服务已经可用。
真实地图、连续地址定位、站点点击和业务闭环验收见
`docs/development/delivery_ui_validation.md` 与 `docs/development/delivery_acceptance.md`。
