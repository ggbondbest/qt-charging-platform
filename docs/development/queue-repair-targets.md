# 排队、报障与目标充电：本地联调说明

## 分支与范围

`feature/queue-repair-targets` 基于已验证的 PR #52 代码开发。三个功能接入真实 TCP、业务服务和 SQLite；Mock 仅用于界面预览，不伪造排队或报障成功。首阶段完成本地验证；提交 PR 前同步了最新 develop，保留 PR #53/#54 的会员中心、每日任务与积分商城功能。

保持 C++17 / Qt 6.2.4 兼容接口。数据库升级为版本 5，旧数据库在事务内增量迁移；保留既有用户、余额、订单、电站、通知及已读状态。无需删除数据库重建。

## 一、虚拟排队

- 电站无空闲电桩时，在电站详情选择具体电桩，点击“加入排队”。
- 每个账号只能有一个有效队列，也不能同时持有未完成订单。
- 同一个电桩按 `entered_at, id` 先到先服务。空闲且站点营业、没有维护任务/故障事件时叫号，电桩预留给队首。
- 叫号确认期限 **60 秒**；确认后创建原有预约和订单，继续 **15 分钟**预约倒计时。计价快照和队列确认同一事务提交。
- 未确认、预约过期或主动退出都会释放名额，后续用户自动获得机会。充电结束视为车位空出，不等待订单支付。
- 客户端顶部出现叫号入口；“我的 → 我的排队”可看位置、前方人数、截止倒计时及退出。
- 管理端“排队与维修 → 实时排队”按电站查看队列，用户手机号脱敏。

状态：`WAITING → CALLED → CONFIRMED`；退出 `LEFT`，超时或身份失效 `EXPIRED`。维修/停站期间不会继续叫号。

## 二、报障与维修

电桩详情的“报障”填写问题类型及 1–200 字说明；“我的 → 我的报障”显示本人记录和更新时间轴。

`SUBMITTED → ACCEPTED → PROCESSING → RESOLVED`

- 提交本身不改变电桩状态。管理员核实受理后，电桩设置为 `OFFLINE`，DTO 附 `maintenance=true`，两端显示“维护中”。
- 有正在充电、真实预约或叫号占用的电桩不能直接中断；管理员会收到资源忙提示。
- 维修按钮必须填写处理说明。使用 `expectedUpdatedAt` 防止覆盖其他管理员的处理；使用 `operationId` 幂等重试。
- 同一电桩多条已确认维修全部完成，并且无其它有效故障/占用且电站营业时，才恢复空闲。
- 普通改状态、重启、旧故障恢复入口不能绕过进行中的维修任务。
- 更新进度会持久化通知报障用户。维修为模拟处理，不控制真实硬件。

## 三、目标充电

在已有预约点击开始充电时，选择按金额、电量或时长；开始后均支持提前手动结束：

| 类型 | 协议 `type` | `value` 单位 | 示例 |
| --- | --- | --- | --- |
| 金额 | `AMOUNT` | 分 | 20 元 = 2000 |
| 电量 | `ENERGY` | Wh | 10 度 = 10000 |
| 时长 | `DURATION` | 秒 | 30 分钟 = 1800 |

`START_CHARGING` 的可选 `target` 只允许 `{type,value}`，值必须为正整数；不传表示兼容原有手动模式。客户端不得上传已充电量、费用或进度。

服务器每秒检查，同时在业务请求前推进状态；客户端关闭或断线不影响自动停止。阈值跨越时截取目标边界，再按预约价格快照和整数规则计费。金额是上限，受最小 Wh 计量精度影响可能少量剩余，但绝不超预算。零电价不支持金额目标。

充电页面显示 `target.completedValue/remainingValue/progressPercent`。订单持久化 `stopReason`：

- `TARGET_AMOUNT`：达到金额目标。
- `TARGET_ENERGY`：达到电量目标。
- `TARGET_DURATION`：达到时长目标。
- `MANUAL`：提前手动结束。

自动停止后仍经过原有“待支付 → 支付 → 完成”流程。重复停止不会重复结算。

## 接口与组织

| 范围 | 接口 |
| --- | --- |
| 用户排队 | `QUEUE_JOIN`、`QUEUE_GET_MINE`、`QUEUE_LEAVE`、`QUEUE_CONFIRM` |
| 用户报障 | `REPAIR_SUBMIT`、`REPAIR_GET_MINE`、`REPAIR_GET` |
| 管理排队 | `queues.list` |
| 管理维修 | `repair_reports.list/get/accept/start/resolve` |
| 实时通知 | `WORKFLOW_SUBSCRIBE` → `WORKFLOW_CHANGED` 事件 |

用户身份由 TCP 会话提供；管理身份由已有 AdminRequestGateway / AdminService 校验，不能通过用户 TCP 冒充管理员。

新增 `QueueService/QueueRepository`、`RepairService/RepairRepository`；目标扩展 `ChargingService/ChargingRepository`，计量辅助位于 `charging_target_repository`。`ServerRuntime` 的工作线程统一执行超时、自动停止和叫号，不在页面线程访问 SQLite。

状态事件只广播无私密内容的失效提示；客户端收到后重新读取本人 DTO。只有主动订阅的新客户端接收事件，旧请求/响应客户端保持兼容；隐藏页面不应持续轮询。断线后需重新登录恢复会话。

新增表：`queue_entries`、`repair_reports`、`repair_timeline`、`repair_operations`、`order_charge_targets`；订单增加 `stop_reason`。

## 本地验收

1. 启动管理端、登录管理员，再启动至少三个客户端（不同手机号）。使用测试电站把所有可用桩占满。
2. A 充电，B/C 按顺序加入 A 的电桩队列；A 停止，B 收到叫号；B 不确认，60 秒后 C 收到叫号。C 确认后可在充电页看到预约倒计时。
3. 选择空闲电桩提交报障，检查仍可用；管理员受理后两端显示维护中，处理完成后用户时间轴更新、电桩恢复。
4. 预约后设置较小金额或短时长目标，观察目标进度、自动结算和订单停止原因；另测手动提前结束及客户端断线后的自动停止。
5. 所有自动化数据库测试仅使用临时数据库。运行 `ctest --test-dir build --output-on-failure`；界面资源检查运行 `bash scripts/verify_qml_routes.sh build/client/charging-client`。

重点测试：`queue_workflow`、`repair_workflow`、`charging_targets`、`workflow_tcp`、`workflow_migration`、`workflow_management_page`，以及原有预约/计费/数据库维护回归。

### 本次验证记录

2026-09-09：完整构建成功，67 项 CTest 全部通过，25 个嵌入式 QML 页面检查通过。页面检查使用显式 Mock，仅证明资源与布局加载；真实业务由 `workflow_tcp` 等临时数据库测试单独验证。另含 `workflow_bridge` 的漏事件补刷/订阅重试测试、`workflow_management_races` 的六类并发/账号切换回归，以及 `qml_workflow_pages` 的真实 TCP 报障提交与时间轴刷新测试。

本地验证使用 Qt 6.12.0；代码遵循 Qt 6.2.4 兼容接口，但本次未运行 Ubuntu / Qt 6.2.4 环境或远端 CI，因此不能把本地通过等同于该环境验收通过。

提交 PR 前同步 develop：68 项 CTest 回归及失败项修复重跑通过，28 个 QML 页面检查通过。会员中心等入口增加后，消息通知点击测试改为按真实元素位置滚动，继续使用真实鼠标点击和原导航断言。
