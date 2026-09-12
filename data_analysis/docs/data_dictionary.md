# 数据字典与关系契约（schema 1.1 / generator 2.0）

完整且精确的字段顺序在 `charging_data/schema.py` 和每个批次的 `schema.json`。
本说明定义字段含义，不能只根据列名自行猜单位。

## 表及关联

| 表 | 一行是什么 / 主键 | 关联与用途 |
| --- | --- | --- |
| cities | 城市；city_id | 五城市中心及 Asia/Shanghai 时区 |
| stations | 模拟电站；station_id | city_id → cities；场景类型、示意坐标、容量、日租金 |
| chargers | 单个并发车位资源；charger_id | station_id → stations；AC/DC、额定功率、模拟型号 |
| users | 虚拟用户；user_id | home_city_id → cities；注册日、用户分群、来源、会员 |
| vehicles | 虚拟车辆；vehicle_id | user_id → users；电池容量 kWh、车辆最大充电 kW |
| vehicle_energy_intervals | 平台外能量变化区间；interval_id | vehicle_id → vehicles；行驶耗电、网外补能及前后 SOC，不计平台营收 |
| tariffs | 模拟运营商小时套餐；tariff_id | city_id+hour 唯一；购电成本、零售电价、服务费，均为分/kWh；非真实地方政策 |
| campaigns | 一段优惠活动；campaign_id | city_id → cities；有效期、每次优惠、预算（分） |
| calendar | 城市一天；city_id+business_date | is_weekend 为字面周六/周日；事件含官方假日/调休及明确模拟的 LOCAL_EXPO |
| weather_hourly | 城市一个小时；city_id+recorded_at | ERA5 再分析温度 °C、湿度 %、天气分类、前一小时总降水 mm（含雪水当量） |
| operating_costs | 站点一天；station_id+business_date | 租金、人工、网络成本，均为分 |
| charging_attempts | 一次初始到访请求；attempt_id | user/vehicle/station；成功后 session_id，预约/排队 ID 可空 |
| reservations | 一次预约；reservation_id | user/station/charger；创建、截止、结局时刻，成功关联 session |
| queue_entries | 一次站内排队；queue_id | user/station；入队位置、叫号、结束、结局，成功关联 session |
| charging_sessions | 一次结束的充电会话；session_id | attempt/user/vehicle/station/charger；充电、停车、计费与 SOC |
| payments | 一次支付或退款事件；payment_id | session/user；多次尝试不等于多笔订单 |
| maintenance_tickets | 一次自动故障工单；ticket_id | charger/station；响应、开始修理、恢复、成本 |
| maintenance_events | 工单一个进度节点；event_id | ticket_id；时间、状态、说明 |
| reviews | 一次自愿评论；review_id | session/user/station；1–5 分及问题分类 |
| charger_telemetry | 一个桩的五分钟区间；charger_id+recorded_at | station/session；状态、能量、购电、在线、电表 |
| battery_samples | 一个充电区间的电池采样；session_id+recorded_at | charger；SOC、电压、电流、温度；仅充电时有数据 |
| anomaly_labels | 一个注入异常标签；label_id | session_id；仅离线评估用，禁止放入模型输入 |
| corruption_log | 一次脏数据注入；corruption_id | table_name+原始 record_id；类型和预期清洗动作 |

一次请求最多一条成功会话；一条会话可能有失败支付、成功支付、后续退款。
聚合支付以后再关联会话，不要让一对多关联把会话电量翻倍。
空字符串表示没有关联/事件未发生；它不是字符串 `null`，也不是零号实体。

## 单位、时间、区间

- `*_cents` 为整数分，`*_cents_per_kwh` 为整数分/kWh；非负，退款用独立事件表达，不写负金额。
- `energy_wh` 是交付电量，`grid_energy_wh` 含模拟充电转换损耗；二者不能相加。
- 每五分钟费用分别按 `(Wh × 分/kWh + 500) // 1000` 四舍五入到分，最后累加会话。
- 会话 `total_fee_cents = electricity_fee_cents + service_fee_cents + parking_fee_cents - discount_cents`。
- `parking_policy_id=site_connector_v2`：住宅/办公/校园 AC 的充后驻留包含在套餐内；其他场景充后 10 分钟免费，超出每分钟 8 分，均为模拟政策。旧 v1 使用原统一规则，校验器按 manifest 区分，不混批次。
- `power_kw` 是五分钟平均功率：`energy_wh × 3600 / interval_seconds / 1000`。
- `meter_wh` 是从本批次开始计量到本区间结束的累计交付电量，初值 0、单调不减。
- `*_at`、`recorded_at`、`registered_at` 是 UTC ISO8601，末尾 `Z`；分片月份、business_date 和电价小时按 UTC+8。
- 区间左闭右开，`ended_at` 起不再充电，`unplugged_at` 起不再占桩。二者不同是为了模拟充后占位。
- `soc_pct` 为百分数，不是 0–1 比例。电池区间采样 SOC 是区间开始值，会话 end_soc 是全部交付完成值。
- 电池电流采用充电为正，电压单位 V、电流 A、温度 °C。串联 96 节及电压曲线是教学简化，不是具体车型 BMS。
- 坐标 WGS84，按城市中心偏移，仅作地图示意；若使用要求 GCJ-02 的地图服务，应先正确转换，不能当真实站点精度。

`vehicle_energy_intervals` 是聚合能量账，不是每次网外插枪或真实出行轨迹。
`电池容量Wh × (end_soc_pct - start_soc_pct) / 100 = external_charge_wh - driving_wh`。
该区间不能与同车平台充电/占位区间重叠；相邻记录和平台会话之间 SOC 连续，允许没有出行的空档。
网外补电只解释后续 SOC，不生成本平台会话、支付、电费或电表值；累计区间不应未经假设按日均摊。
车辆不能因每单重采样而瞬间掉电；当短时间不足以进行一次网外补能时，模拟车辆最多驾驶到预留电量。

天气来源按 manifest.weather_source_rows 计数。交付的 180 天及 7 天批次全部使用缓存 ERA5；
自定义日期超出缓存时使用标明的季节模拟回退，不能叫实测天气。`rainfall_mm` 保留旧列名但不是“仅雨量”。
须保留 [Open-Meteo](https://open-meteo.com/) / ECMWF / C3S 署名及 [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) 许可。
参考缓存原始 WMO 码在正式表转换为 CLEAR / CLOUD / FOG / RAIN / SNOW。

`calendar.scenario_event` 中 `PUBLIC_HOLIDAY_*`、`ADJUSTED_WORKDAY` 的日期来自
[官方通知](https://www.beijing.gov.cn/fuwu/bmfw/sy/jrts/202511/t20251104_4258838.html) 与 `config/calendar_reference.json`。
`demand_multiplier` 只含模拟展会/相关日波动；节假日按站型的影响另在行为模型中计算，不能把这个数当最终到访概率。
配置覆盖外仅按星期计算，不声称已具备其他年份的法定日历。

## 状态与结局

- 遥测：AVAILABLE / CHARGING / RESERVED / OCCUPIED / MAINTENANCE / OFFLINE；六状态互斥，维护仍可在线。
- 请求：STARTED / FAILED / ABANDONED / RESERVATION_CANCELLED / RESERVATION_EXPIRED / QUEUED。
- 请求 `attempted_at` 是初次到访，不是插枪或握手时间。未充成可以没有 charger_id 或 session_id。
- 预约：CONFIRMED / CANCELLED / EXPIRED。本版周期结束前停止新请求，不输出未完预约。
- 队列：SERVED / ABANDONED / CALL_EXPIRED / WAITING；按站内 FIFO 分配，叫号保留 5 分钟。
- 会话：COMPLETED（已支付）/ WAITING_PAYMENT（充电已结束但未付）；都不是正在充电。
- 停止原因：TARGET_REACHED / USER_STOPPED / DURATION_LIMIT（分站型停留/充电时长上限或批次收尾，不再是统一 5 小时）。
- `target_mode` 本版固定 ENERGY，`target_value` 单位 **Wh**。用户提前停止也保留原始目标，不能把已充量改成目标。
- 支付：transaction_type 为 PAYMENT / REFUND；status 为 SUCCESS / FAILED。
- 工单：RESOLVED / IN_PROGRESS；事件 SUBMITTED → ACCEPTED → IN_PROGRESS → RESOLVED。
- 故障：CONNECTOR / COMMUNICATION / COOLING / POWER_MODULE。通信故障映射 OFFLINE，其他映射 MAINTENANCE。
- 异常标签：POWER_DERATING / EARLY_STOP / THERMAL_STRESS；未异常不发标签。这不是实际故障诊断真值。

## 脏数据与对照结果

只对 `charging_sessions` 注入五类脏操作：

| 操作 | 原始表变化 | Spark 处理 |
| --- | --- | --- |
| STATUS_FORMAT | 原行 status 改为带空白小写 | trim + uppercase 恢复 |
| DUPLICATE | 额外插入相同合法副本 | 按 session_id 确定性去重 |
| NEGATIVE_FEE | 额外插入新 BAD ID 且费用为负 | 隔离，保留合法原行 |
| UNKNOWN_STATION | 额外插入新 BAD ID 且电站不存在 | 隔离，保留合法原行 |
| MISSING_ID | 额外插入缺失 ID 的副本 | 隔离，保留合法原行 |

校验器对脏清单逐项核对，不是发现坏行就一概忽略。清洗会话数应等于 manifest.canonical_session_count。
脏数据/异常标签只供验收，Spark 清洗不能读取标签把错误答案直接还原；模型也禁止读取标签当特征。

`reference_aggregates` 是 Python 模拟器产生的控制值，不是需要二次累计的原始数据：

- station_hourly：每站每小时一行；Wh、平均 kW、六状态采样数、capacity、最后一个采样时刻空闲数。
  完整一小时每桩 12 行、每站 36 行；最后样本通常为 hh:55，**不等于下一小时整点预测值**。
- station_daily：每站每天一行；电量/购电按区间发生日，会话数按结束日，支付/退款按事件日，维修成本按恢复日。
  `completed_sessions` 表示已结束充电会话（含待支付），页面应叫“结束会话数”。

## 有意简化的部分

按场所、用途和功率兼容性加权选择可用桩；同类等价设备不再永远优先列表第一台。
FIFO 仍按站内排队顺序处理，品牌不影响选择；不能据利用率排名声称某品牌更好。
维护只在空闲时自动检测故障，不等同于用户一提交报障就停用；没有模拟在充电途中突然硬件断电。
本版桩功率合计低于站点容量，没有超容量事故。分群、会员和活动预算可统计，但不能据此得出真实营销因果效果。
遥测没有人为缺包/迟到，脏记录只在会话表；数据质量演示应明确此范围。
每城五种场景的布置是可比教学样本，不是城市市场调查。没有跨城市行驶；车辆可有明确记账的网外补能。
批次从空系统启动，期末会截短驻留，行为报告排除首尾业务日；全量财务对账仍保留全部区间。
