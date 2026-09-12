# 管理端扩展数据与接口（Schema v4）

本文补充 [管理端 Service 契约](admin_service_contract.md)，对应《扩展的数据与接口需求》的 P0 和交互补充。
仅补齐已确认能力；不包含 P1 的单桩新增、风险标签、退款/支付扩展、运行日志和自定义趋势。
Qt 兼容基线仍为 **6.2.4 / C++17**，未增加第三方依赖。

## 1. 调用、安全与数据单位

页面 → `AdminRequestGateway` → `ServerRuntime` 服务线程 → `AdminService`
→ `AdminRepository` 及领域 helper → SQLite。结果经原请求编号异步返回页面。
管理 action 不开放给用户 TCP 身份；页面不直接访问数据库或创建 Socket。

- 成功：`{success:true,data:{...}}`；失败：`{success:false,error:{code,message}}`。
- ID 为正十进制字符串，不接受 JSON 数字、前导零、负数。
- 金额为整数分，功率整数 W、电量整数 Wh、时长整数秒；不使用浮点元参与计费。
- 时间为 UTC `yyyy-MM-ddTHH:mm:ss.zzzZ`。日期筛选先将北京时间日界转换为 UTC，使用 `[from,to)`。
- 缺失事实返回 `null`，页面显示 `—`；不得从总价反推收费项目、从版本时间猜测事件发生时间。
- 不返回密码散列、盐、会话 token、SQL、堆栈、原始第三方报文或完整交易号。
  用户手机号保持脱敏；电站联系手机号仅受保护的管理员详情可查看完整值。

所有新增接口沿用管理员会话校验和安全错误：未认证/过期会话不可查询。
`gateway.adminId()` 仅提供当前已认证管理员的公开 ID，供界面核对重试归属；不能由页面设置身份或替代服务端鉴权。

## 2. 电站联系信息

`stations.list/get` 的 item 增加：

| 字段 | 类型 / 长度 | 默认及旧数据 |
| --- | --- | --- |
| city | string，去首尾空白后 1–64 字 | null |
| district | string，去首尾空白后 1–64 字 | null |
| contactName | string，去首尾空白后 1–64 字 | null |
| contactPhone | string，11 位大陆手机号，正则 `^1[3-9][0-9]{9}$` | null |

列表将电话显示为 `138****8000`；`stations.get` 在管理员鉴权后返回完整联系电话。
管理表单新增、编辑时上传以上四项；编辑必须先加载详情，不能把列表中的脱敏电话写回数据库。
表单先去除首尾空白；直接调用 API 时带首尾空白会被拒绝，不由服务端静默改写。

`station.create/edit` 接收并持久化四项，沿用原站点名称、地址、坐标、电价等必需字段。
为兼容已有调用方，API 允许省略新增字段：新建时省略为 null，编辑时省略保留原值；
**传入空串、null 或非法格式会拒绝，不能静默忽略。** 新管理表单要求四项填写完整。
旧站点缺失资料由管理员补录，不从地址猜测区域，不编造负责人/电话。

`station.create` 使用 `operationId`（创建前没有版本）；`station.edit` 另需 `id,expectedUpdatedAt`。
新字段纳入请求指纹、事务、版本冲突检查与审计；同 operationId 不允许改变内容。

## 3. 实时监测：读取最后一次服务端模拟样本

`chargers.runtime.get {id}` → `data.item`：

| 字段 | 类型 / 可空性 | 含义 |
| --- | --- | --- |
| chargerId, status | string，非空 | 电桩 ID、当前状态 |
| sessionId | string / null | 当前 CHARGING 订单 ID；不是硬件会话凭据 |
| capturedAt | UTC 时间 / null | 最后一次已持久化的模拟计量采样时间 |
| currentPowerWatts | 正整数 / null | 采样功率 |
| energyWh, chargeSeconds | 非负整数 / null | 采样时累计电量、时长 |
| currentAmountCents | 非负整数 / null | 服务端按该订单固定费率计算的暂估电费 |
| lastHeartbeatAt | UTC 时间 / null | 本版未接硬件心跳，始终 null |
| source, simulated | `SIMULATED_METER`、true | 明确数据来源是服务端模拟计量 |
| estimated | bool，可省略 | 有充电样本时为 true |

非 CHARGING 时全部实时值及 sessionId 为 null。充电中尚无样本仍返回 null；
存在样本但无历史快照时金额为 null，不按当前站点电价补算。
刷新详情只读数据库，不推进采样、不生成心跳、不把查询时间当采样时间。
充电业务的计量采样会更新订单累计电量、金额、遥测字段；停止充电后固定最终值。
页面首版手动刷新，显示“服务端模拟采样”和“暂估金额”，不宣称硬件实时推送。

## 4. 独立异常事件与恢复

新增 `charger_exceptions`，一根桩可有多条事件：

| 字段 | 类型 / 枚举 / 默认 |
| --- | --- |
| id, chargerId | 非空十进制字符串 |
| code | `SIMULATED_FAULT` / `SIMULATED_OFFLINE` |
| severity | `CRITICAL` / `WARNING` |
| safeSummary | 服务端枚举生成的安全摘要，非空，最多 256 字 |
| status | `ACTIVE` / `ACKNOWLEDGED` / `RECOVERING` / `RECOVERED`，新事件 ACTIVE |
| occurredAt, updatedAt | 非空 UTC 时间；前者是发生时间，后者是独立并发版本 |
| acknowledgedAt, recoveredAt | UTC 时间 / null，不从 charger.updatedAt 补齐 |
| recoverable | bool，新事件 true，恢复后 false |
| recoveryAction | 本版仅 `SIMULATE_RESTORE` |
| recoveredByAdminId, recoveryCommandId | string / null，恢复前 null |
| recoveryMessage | string / null，最多 256 字，仅服务端安全说明 |
| simulated | true |

`charger.status` 把桩变更为 FAULT/OFFLINE 时，在同一事务写入新模拟事件。
重复设置相同状态不会重复造事件；旧故障状态不补造时间未知的历史事件。

| action | 输入 | 输出 data |
| --- | --- | --- |
| charger_exceptions.list | page、pageSize；可选 chargerId、status、severity、occurredAtFrom、occurredAtTo | items、total、page、pageSize |
| charger_exceptions.get | id（异常 ID） | item |
| charger_exceptions.recover | id（异常 ID）、operationId、expectedUpdatedAt（异常版本）、recoveryAction | item、commandId、idempotent、simulated |

异常列表按 `occurredAt DESC,id DESC` 稳定排序，时间范围半开。
`chargers.list/get.activeException` 为最新一条活动事件摘要（无则 null），包含上述事件 ID、
状态、枚举、摘要、发生时间、recoverable、updatedAt 和 recoveryAction 等安全字段。
Dashboard、列表、详情和恢复按钮使用该事件 ID 与版本，不把电桩 ID 当异常 ID。
`abnormalOnly=true` 包含 FAULT/OFFLINE 桩或仍有活动事件的桩。

### 本版恢复的明确语义

- 是**同步完成的受控状态模拟**：允许 ACTIVE/ACKNOWLEDGED → RECOVERED。
  没有异步硬件队列；不会先回 RECOVERING 再假定成功。
- 当前事件不可恢复、状态不允许、站点停用：`INVALID_STATE_TRANSITION`。
  充电、预约或存在占用订单/预约：`RESOURCE_BUSY`。版本过期：`CONFLICT`。
- 有其他活动异常时只恢复本事件，桩仍不可用；最后一条活动异常恢复后才改 AVAILABLE。
  页面展示服务端 `recoveryMessage`，不能只因请求成功就宣称整桩已恢复。
- 同一个 operationId、同一请求重放，返回原 commandId 和 `idempotent:true`，不再写一次日志。
  状态、事件、审计和幂等记录在同一事务中；任何步骤失败整体回滚。
- `charger.restart` 不是恢复接口，仍为模拟动作；存在活动异常时禁止用重启绕过恢复。
- 页面写请求提交后锁住相关动作，列表刷新不会解除锁。超时表示“结果未知”，
  可使用原 action、原目标、原版本和原 operationId 核对重试，不因切换选择而改变原命令。
  必须是原管理员会话身份；换管理员不能重放上一位管理员的未决操作。

## 5. 订单费用与固定费率

仅 `orders.get.data.item` 增加费用对象；列表保留轻量 DTO，不要求列表/详情所有字段完全相等。

- `billingAvailability`：`AVAILABLE/UNAVAILABLE`；`estimated`：充电中 true，否则 false。
- `feeBreakdown`：`energyFeeCents,serviceFeeCents,parkingFeeCents,discountCents,payableCents,paidCents`
  均为非负整数分；`currency="CNY"`。
- `pricingSnapshot`：`version="energy-only-v1"`、`capturedAt`、`segments[]`。
  每段为 `startAt,endAt,energyWh,unitPriceCentsPerKwh,amountCents`。

当前真实实现只有电费，因此服务费、停车费、优惠为 **0**：
`payableCents = energyFeeCents + serviceFeeCents + parkingFeeCents - discountCents`。
电费按整数 Wh × 固定分/kWh 除以 1000，半分向上取整，并检查整数溢出。
完成支付的订单 paidCents 为已付电费，未支付为 0；这不代表已实现退款或多支付渠道。

新预约创建时固化费率；未启动的旧预约启动时使用其订单已有固定费率补建快照。
已充电/结算且没有快照的旧订单不补造收费明细：费用对象均 null、UNAVAILABLE。
预约未开始时 segments 为空；开始后为单一固定费率段，结束时间使用采样时间/停止时间，
不使用管理页面查询时间。后续电站调价不修改历史订单费率。
如存储金额与快照不一致，返回安全错误，不把不一致明细当可信结果显示。

## 6. 查询、选项与管理页面

以下新增筛选在 `.list` 与 `.summary` 中使用同一条件构造，全部按 AND 组合。
summary 不接受 page/pageSize/sort，统计整个筛选集合；列表与 total 在同一读事务内取得。

| 接口 | 新增输入与匹配规则 |
| --- | --- |
| stations.list/summary | city、district 精确匹配；idleOnly 为 bool，默认 false；true 表示站内没有 RESERVED/CHARGING 桩，与 ACTIVE/INACTIVE 独立组合 |
| chargers.list/summary | powerWatts 精确；或 minPowerWatts/maxPowerWatts 闭区间，范围 1–1000000 W；精确和区间不能混传 |
| users.list/summary | createdAtFrom/To 半开；minBalanceCents/maxBalanceCents 闭区间，0–9007199254740991 |
| orders.list/summary | orderNo 精确（1–64 字）；userKeyword 按昵称字面包含（1–64 字）；phone 为完整 11 位大陆手机号精确匹配，三个非空框同时上传 |
| operation_logs.list/summary | adminId 精确 ID；action 为服务端元数据中的 action 值；keyword 仍搜索操作/目标文本，不承担管理员选择 |

“无占用（全部桩）”不等于“运营中”：也可能包括没有桩、离线或故障桩的站点，
只承诺无 RESERVED/CHARGING，不能据此宣称存在可预约设备。
字符串按字面匹配，`%`、`_` 不作为通配符。数值/时间上下界倒置拒绝，未知字段拒绝。

| action | 请求 | 每个选项 item |
| --- | --- | --- |
| stations.options | keyword、page、pageSize | id、code、name |
| chargers.options | 同上，可选 stationId | id、code、name（桩号）、stationId、stationName |
| admins.options | keyword、page、pageSize | id、code（用户名）、name（显示名），不含认证数据 |

keyword 0–64 字；默认 page=1、pageSize=20，允许 page 1–1000000、pageSize 1–100。
选项按 ID 升序，统一返回 items/total/page/pageSize；keyword 对编号/名称字面包含，
电桩还匹配所属站名，管理员匹配用户名/显示名。
管理端下拉以每页 50 条搜索、加载更多，不再截断为前 100 条；搜索节流 300ms。
刷新保留选择；从 Dashboard 跳到不在首屏的站点，使用指定 ID 详情补齐选项。
过期响应不覆盖新关键词结果；刷新失败保留旧数据并提示。

`operation_logs.actions {}` 返回 `items[{action,valueLabel,category}]`，当前 7 项：
station.create/edit/status（STATION）、user.status（USER）、charger.status/restart（CHARGER）、
charger_exceptions.recover（EXCEPTION）。UI 用 action/category 值，不从中文标签反推。
历史其他 action 仍可被精确查询，但不把任意历史日志内容当元数据回传。
充值状态保持 SUCCESS/FAILED，管理页移除“处理中”，本版不新增异步充值协议。

## 7. 升级与验收

Schema v4 新增四个站点联系列、两个订单遥测列、order_pricing_snapshots 和 charger_exceptions 两表。
受支持的 v1/v2/v3 数据库由服务端启动自动原地迁移；DDL、索引、列和版本同事务提交，
失败完整回滚。旧版备份先在临时副本完成验证/升级，不能用清库方式升级。
建议升级前保留备份。此次开发/测试仅操作临时测试数据库，不修改现用演示库。
详见 [数据库说明](../../database/README.md)。

重点回归：

```bash
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
bash scripts/verify_database.sh
bash scripts/verify_city_demo_data.sh
```

新增查询、页面交互、异常、账单测试覆盖：旧 NULL 数据编辑、手机号/交易号脱敏、
订单组合条件、时间/金额边界、超过一页的搜索与选择、模拟采样不冒充心跳、
版本冲突、占用状态、幂等重放、多事件恢复、审计失败回滚、历史费率不随调价改变及旧库迁移。
精确 Qt 6.2.4 / Ubuntu 22.04 构建与完整测试由仓库 CI 验证。
