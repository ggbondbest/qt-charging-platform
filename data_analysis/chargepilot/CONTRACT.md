# ChargePilot 接口与集成说明

ChargePilot 使用 Vue、FastAPI 和独立运营 MySQL，API 前缀为 `/api/v1/chargepilot`。电站、充电、支付和积分均为模拟演示；历史预测与本次用户行程分开维护，不连接真实电桩、真实支付或一期 Qt。启动配置见 [README](README.md)，本子系统交互式接口文档为 `/api/v1/chargepilot/docs`；统一服务的统计/智能分析文档仍为 `/docs`。

## 通用约定

成功响应为 `{"data":...,"meta":{"dataSource":"SIMULATED","clockTime":"UTC时间戳","mode":"REPLAY_DEMO"}}`；下表的返回值均指 `data`。失败响应为 `{"error":{"code":"…","message":"…"}}`，同时使用对应 HTTP 状态码。

时间以 UTC 传输，界面按 `Asia/Shanghai` 显示；金额为人民币元，电量为 kWh，功率为 kW，距离为 km，预测等待/行驶时间为分钟，充电计时为秒。用户接口使用 `Authorization: Bearer <token>`，管理员接口使用 `X-Admin-Token: <token>`。演示会话有效期为24个**真实小时**，与加速时钟无关。

新数据库从 `2026-05-05T00:00:00Z`（北京时间08:00）开始，默认10倍速、未暂停。时钟单调前进，后台约每秒同步，读取/操作也会处理到期事件；重启保留已保存的时钟、订单和积分。可见页面每2秒刷新站点和个人行程，隐藏或卸载时停止轮询。会话创建按来源IP、推荐和路线按用户分别限制每60真实秒20次。

## 公共与用户接口

| 方法与路径 | 鉴权 | 请求与返回 |
| --- | --- | --- |
| `GET /health` | 无 | `{status:"ok"或"degraded",database:"MySQL",arrivalModel,loadModel}`。 |
| `GET /bootstrap` | 无 | `{cities,stations,clock,modelStatus,provenance}`；城市含 `cityId,cityName,latitude,longitude`。 |
| `GET /stations?cityId=DL` | 无 | 站点数组；不传城市返回全部站点，未知城市返回空数组。 |
| `POST /sessions` | 无 | `{"name":"演示用户"}`，昵称1–40字符且非空白；返回 `{token,user:{userId,name,points},sessionExpiresAt}`。 |
| `POST /recommendations` | 用户 | 请求和预测字段见下一节。 |
| `POST /recommendations/{id}/select` | 用户 | `{"stationId":"ST-DL-01"}`；返回行程。仅可选择本人未过期推荐内的候选，同一推荐重复选择同站返回原行程。 |
| `GET /me` | 用户 | `{user,trips,ledger}`；行程按创建顺序倒序，账本按入账时间倒序。 |
| `POST /trips/{id}/{action}` | 用户 | `action` 为 `arrive/confirm/start/stop/pay/cancel`；返回更新后的行程。 |
| `POST /route` | 用户 | `{origin:{latitude,longitude},stationId}`；返回 `{routeSource,coordinates,notice}`，每个坐标点为 `[纬度,经度]`。 |
| `GET /models` | 无 | `{status,arrival:{name,metadata,metrics},load,provenance}`，包含实际模型、来源和评价。 |
| `GET /load?stationId=ST-DL-01&horizonHours=6` | 无 | 小时负荷预测；跨度仅支持1、6、24，默认6，未就绪返回503。 |
| `GET /experiments` | 无 | 实验报告或状态对象；异步约定见后文。 |

站点字段包括 `stationId,cityId,stationName,siteType,latitude,longitude,capacity,powerKw,ratedCapacityKw,powerAssumption,pricePerKwh`，以及 `currentFree,queued,committed,charging,enRoute,backgroundBusy,backgroundReleaseAt`。`committed` 只包含已叫号/已预约占桩，排队和在途单独计数；`currentFree = capacity − backgroundBusy − committed − charging`。

`powerKw` 是站点总额定功率除以容量的虚拟均分单桩功率，`ratedCapacityKw` 保留总功率，`powerAssumption=VIRTUAL_EQUAL_SHARE`。目录价格是展示基准，行程计费以推荐候选内冻结的报价为准。

## 推荐与预测字段

推荐请求示例：

```json
{"cityId":"DL","origin":{"latitude":38.914,"longitude":121.6147},"energyKwh":20,"maxEtaMinutes":60}
```

目标电量允许1–100kWh，默认20；最大ETA允许5–60分钟，默认60。起点需在所选城市中心100km以内，每座候选使用自己的ETA，超出上限不进入结果。

返回 `recommendationId,createdAt,expiresAt,referenceTime,origin,cityId,energyKwh,candidates,nearestComparison,warnings`。推荐生成后5个回放分钟过期；修改城市、起点或需求后应重新推荐。`nearestComparison` 包含最近/推荐站ID、站名、两站预测总用时和 `savedMinutes`，后者允许为负，不是实际节约量。

| 候选字段 | 含义 |
| --- | --- |
| `rank,score,scoreBreakdown,reasons` | 从1开始的名次、0–100评分、六维加权贡献和说明；维度为 `availability,wait,travel,price,power,balance`。 |
| `distanceKm,etaMinutes,routeSource` | 逐站路程、ETA和来源；降级采用直线距离×1.3、28km/h及2分钟准备时间的情景估计。 |
| `currentFree` | 当前运营沙盒可用桩数，与历史原始模型输入分开维护。 |
| `availabilityDistribution,expectedFree,availableProbability` | 索引0至容量的空闲桩数概率分布、其期望及至少一桩可用概率；期望可以为小数，`availableProbability=1−P(0)`。 |
| `waitMinutes,waitP90Minutes,serviceProbability` | 成功获得非预约充电服务条件下的平均/P90等待估计及服务成功概率；失败样本的等待没有填成零。 |
| `arrivalTime,forecastTime,resolutionMinutes,featureAsOf` | 精确预计到达、所属预测时间槽起点、5分钟分辨率及最后一个完整历史采样的起点。 |
| `rawModelPrediction,operationalAdjustment` | 原始模型输出与当前订单/排队/在途承诺的保守规则修正；修正后的概率不能直接继承原始模型的测试校准指标。 |
| `pricePerKwh,pricePolicy` | 按预计到达本地小时选取并冻结的报价；`pricePolicy=ARRIVAL_TARIFF_FIXED_QUOTE`。选择行程后不随新推荐或电价时段重算该报价。 |
| `powerKw,loadRatio,chargingMinutes,totalMinutes` | 虚拟单桩功率、排序忙碌度、目标充电时长和ETA＋条件等待＋充电的预测总时长。 |
| `rewardPoints,rewardMultiplier` | 本次推荐冻结的候选积分及倍率，不等于已到账积分。 |

例如到达 `00:12:42Z` 对应 `forecastTime=00:10:00Z` 的5分钟状态槽。模型只使用出发之前已完成的遥测区间，不读取同刻到访造成的占桩、未来离场或未来失败结果。原始模型与基线的真实评价及限制见 [模型说明](ml/README.md)。

腾讯启用时，ETA/路线来自当前路况，充电状态仍是历史模拟回放，两者不是同日实测。`POST /route` 无道路服务时返回 `routeSource=STRAIGHT_LINE_DEMO` 的演示连线和说明；有腾讯路线时为 `TENCENT_CURRENT_TRAFFIC`。前端必须展示来源和 `notice`。

## 行程、排队、支付与积分

每用户最多有一笔活动行程，`PENDING_PAYMENT` 也算未完成。选择后创建 `EN_ROUTE`；`arrivalEligibleAt` 为选择时刻加候选ETA，提前调用 `arrive` 返回409。

| 当前状态 | 操作/事件 | 结果 |
| --- | --- | --- |
| `EN_ROUTE` | `arrive` | 有空桩且无人排队进入 `RESERVED`，否则进入站内FIFO `QUEUED`。 |
| `QUEUED` | 桩释放后自动叫号 | 队首进入 `CALLED`，占住一个资源并设置 `callExpiresAt`。 |
| `CALLED` | `confirm` | 进入 `RESERVED`，设置 `reservationExpiresAt`。 |
| `RESERVED` | `start` | 进入 `CHARGING`，记录实际到站等待。 |
| `CHARGING` | 达到目标自动停止或 `stop` | 进入 `PENDING_PAYMENT`，立即释放电桩。 |
| `PENDING_PAYMENT` | `pay` | 模拟支付并进入 `COMPLETED`，按资格决定是否发积分。 |
| `EN_ROUTE/QUEUED/CALLED/RESERVED` | `cancel` | 进入 `CANCELLED`，不发积分。 |
| `CALLED/RESERVED` | 确认/预约超时 | 进入 `EXPIRED` 并释放资源。 |
| `EN_ROUTE/QUEUED` | 行程奖励资格期限到期 | 进入 `EXPIRED`。 |

动作可以省略body或发送 `{}`；只有 `stop` 另外接受 `{"reason":"MANUAL"}`。客户端不能提交实际电量、时长、金额、余额或奖励。重复支付不会重复付款或发积分；对应动作已完成时的重试按状态机幂等返回，非法状态转换返回409。

行程包含 `tripId,recommendationId,stationId,stationName,status,origin,createdAt,arrivalEligibleAt,arrivedAt,targetEnergyKwh,energyKwh,chargingSeconds,amount,pricePerKwh,powerKw,queuePosition,peopleAhead,callExpiresAt,reservationExpiresAt,rewardExpiresAt,rewardPoints,awardedPoints,rewardStatus,stopReason,paidAt,actualFreeAtArrival,actualWaitMinutes,events` 等字段。非排队状态的 `queuePosition` 为 `null`；未开始服务的实际等待也为 `null`。

事件为 `{eventId,type,status,createdAt,details}`；账本为 `{tripId,stationId,points,amount,createdAt,businessDate}`，只记录实际发放的正积分。积分在实际充电停止且完成正金额模拟支付后发放，需满足冻结的最低电量/时长、资格期限和候选奖励；发奖还受当日剩余预算约束，可能部分发放。以 `awardedPoints/rewardStatus` 为准。业务日按上海时区计算，点击或选择推荐本身不发积分。

## 管理接口与异步实验

本节均要求管理员令牌；未配置管理员功能返回503，令牌错误返回403。

| 方法与路径 | 请求与返回 |
| --- | --- |
| `GET /admin` | `{clock,config,stations,counts,baselineInitialized}`；计数含用户、推荐、行程、支付及各行程状态。 |
| `PATCH /admin/config` | 非空配置补丁，返回完整配置；未知字段拒绝。修改权重需完整六维且总和为1，修改奖励需同时传 `first,second`。 |
| `POST /admin/clock` | `{"action":"advance","seconds":300}` 或 `{"action":"configure","speed":10,"paused":false}`；返回时钟。HTTP单次推进1–86400秒，倍率0–3600，只能向前推进。 |
| `POST /admin/stations/{id}/background` | `{busyCount,releaseAfterSeconds}`，返回站点。HTTP释放时长1–86400秒，背景不得挤占已叫号、预约或充电的用户资源。 |
| `GET /admin/feedback` | `{records,dataSource:SIMULATED_OPERATIONAL_FEEDBACK,note}`；记录推荐/行程/用户/站点ID、名次、选择/到达/开始时刻、实际空闲/等待、电量、金额、状态和积分，不含昵称或令牌。 |
| `POST /admin/experiments` | 发送JSON `{}` 或 `{"users":1000,"seed":42}`；人数100–2000，种子0–2147483647。立即返回HTTP202和 `{"status":"RUNNING"}`；当前进程已有实验运行时返回409。 |

默认配置如下；旧推荐保存其当时的奖励及资格快照，新配置不追溯修改已保存候选，当前每日预算在实际发奖时检查。

```json
{
  "weights": {"availability":0.30,"wait":0.25,"travel":0.20,"price":0.10,"power":0.10,"balance":0.05},
  "rewards": {"first":30,"second":10},
  "minimumEnergyKwh": 1, "minimumChargingSeconds": 60, "dailyRewardBudget": 10000,
  "callTimeoutSeconds": 60, "reservationTimeoutSeconds": 900, "rewardEligibilitySeconds": 86400
}
```

提交实验后轮询 `GET /experiments`：`RUNNING` 为计算中，`FAILED` 为本次失败，`NOT_RUN` 为无报告，`STALE` 表示报告的模型工件哈希与当前模型不符。完成后返回 `status=READY` 的真实报告，含 `users,seed,simulatorVersion,policyConfig,requestHash,modelArtifactSha256,policies,changes,notes`。管理端实验冻结启动时的权重/积分配置，结果落盘；状态不提供独立jobId。

实验使用同请求、同背景、两份资源账本及固定承诺租约，未来实际状态只供环境结算，不等同业务到站FIFO或现场因果试验。`meanTotalMinutes` 包含失败请求的行驶＋等待，`meanCompletedJourneyMinutes` 才是成功行程的平均总时长；`utilizationStd/maxUtilization` 表示含维护、预留、占位的资源忙碌度，不是纯充电利用率。仿真奖励不写入演示账户；反馈导出也不会自动更新已评估模型。

## 模型与服务接入

`ArrivalPredictor(output_dir)` 提供 `.metadata/.catalog/.cities/.bounds`，以及 `.predict(station_id,reference_time,eta_minutes)`、`.snapshot(reference_time)`、`.price(station_id,timestamp)`。`.actual()` 仅供离线仿真，不是HTTP接口，不能用于推荐特征。模型、回放缓存和元数据绑定来源哈希；加载前核对工件完整性及scikit-learn版本。

小时负荷接口复用PR66模型，参考时刻向下取整到小时，使用之前连续24个完整小时预测。返回 `points:[{timestamp,value}],unit:kW,modelId,modelVersion,referenceTime,stationId,historyHours` 等；时间点表示对应预测小时的起点。工件缺失/校验失败时明确 `NOT_READY`/503，历史不足返回409，不生成替代曲线。

运营MySQL只保存独立的 `cp_*` 业务表，保留已有数据。历史快照仅在初次初始化时建立背景占用，之后按模拟释放事件和用户操作演进，不持续覆盖用户分配。Vue开发服务器通过 `/api` 代理后端，构建后的静态页面可由同一FastAPI提供。密钥和数据库凭证由后端环境配置，前端不内置。

常见错误：会话无效401、管理员无权限403、不可见的行程/推荐或不存在的资源404、状态/容量/过期冲突409、HTTP字段校验422、限流429、模型或服务未就绪503；配置等业务参数还可能返回400。客户端应按HTTP状态及 `error.code` 处理，展示服务端说明，不推断成功或补造预测。
