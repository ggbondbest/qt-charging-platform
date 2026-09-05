# 管理端 Service 与异步接口

## 范围与现状

基于 develop `93788c2` 核对并实现图片中的 Service 职责。Qt 6.2.4 / C++17。

已有的数据库查询和 ServerRuntime 工作线程保留。新增 AdminService、AdminRepository、
AdminRequestGateway，管理登录页已接真实认证并提供退出登录。其他管理页面的列表、图表和
按钮仍由管理端页面负责人接入本文接口；它们原有的 Mock 显示不代表真实业务执行。
本 PR 不增加订单编辑、实际硬件控制、第三方支付或管理员账号维护页面。

## 调用链与线程

页面 → AdminRequestGateway（GUI）→ ServerRuntime 排队请求 → AdminService（服务线程）
→ AdminRepository / DashboardRepository → SQLite → 排队响应 → 原页面。

所有 SQLite 连接、查询和 Service 都在 ServerThread::run 内构造、使用和销毁。
管理接口不注册到用户 TCP RequestDispatcher；手机号登录身份不能充当管理员。
当前只有 GUI + 单个服务工作线程，不是每客户端一个线程。阻塞 SQL 不阻塞 GUI，
但会延迟其他业务；列表最多 100 条，管理写事务短事务化。

## 页面接入

MainWindow::adminGateway() 返回窗口共用的网关；不要给每个页面新建网关。
网关持有会话能力，页面不提交 adminId，也不接触密码散列或 sessionToken。

```cpp
connect(gateway, &AdminRequestGateway::finished, this,
        [this](const QString& id, const QJsonObject& reply) {
    if (id != requestId_) return;
    if (!reply.value("success").toBool()) {
        // 显示 reply.error.message；写超时先核对，不生成新操作编号盲重试。
        return;
    }
    const auto result = reply.value("data").toObject();
    // 更新当前页面；不要捕获已经销毁的 QWidget。
});
requestId_ = gateway->request("stations.list",
    {{"page", 1}, {"pageSize", 20}, {"sort", "idDesc"}}, this, "station-list");
```

- owner 必须是 GUI 线程对象。销毁 owner 后不再回调。
- 相同 owner + 非空 key 只交付最新请求的结果；只抑制旧结果，不撤销已开始的写事务。
- 默认超时 10 秒，可设 1–60000 毫秒；最多 128 个待处理请求。返回空编号表示未受理。
- 超时未开始的任务由工作线程拒绝；已开始任务允许完成原子提交，迟到结果不更新页面。
- 登录被取消、超时或覆盖后，迟到登录能力会被撤销。退出使本窗口所有待处理结果失效。
- 运行时停止返回 UNAVAILABLE 并清理会话。不同窗口请求编号隔离。

## 管理员认证与权限

`auth.login {username,password}` 校验 admins 的 SHA256_SALTED（salt + ':' + password），
沿用现有数据库散列约定，不把演示账号硬编码为绕过认证的条件。
只有 `--demo-seed` 初始化过的演示数据库才有 admin / 123456。该简化散列仅用于实训，
生产部署需迁移到专用慢密码散列方案，不应沿用演示口令。

返回安全 admin DTO：id、username、displayName、permission=ADMIN。
现有表没有角色列，所有 ACTIVE 管理员拥有上述管理权限；不存在客户端自报角色机制。
连续 5 次错误后限制登录 30 秒；当前为本服务实例共享限制。

会话随机生成，空闲 30 分钟、最长 8 小时；每次调用重新核验账号启用状态与凭据指纹。
禁用、修改凭据、退出或服务停止使会话失效。`auth.check {}` 不延长空闲时间，网关每
30 秒检查一次，失效时发出 authenticationChanged(false)。`gateway->logout()` 用于退出。

## 统一返回与查询

成功：`{success:true,data:{...}}`。失败：`{success:false,error:{code,message}}`。
不返回 SQL、路径、异常原文、密码散列或盐。ID 使用正十进制字符串；金额分、电量 Wh、
时长秒使用精确整数。用户手机号统一返回 `138****8000`，包括关联订单和充值摘要。

| action | 输入 | data |
| --- | --- | --- |
| dashboard.get | days=7 或 30，默认 7 | 统计字段、trend、timeZone、observedAt、onlineRatio、abnormalChargers、latestOrders |
| stations.list / get | 通用列表条件 / id | 分页站点 / item，含总桩数、空闲桩数、电价、updatedAt |
| chargers.list / get | 通用条件，可加 stationId、type、abnormalOnly / id | 分页电桩 / item，含站名、功率、累计数据、updatedAt、exceptionType |
| users.list / get | 通用条件 / id | 安全用户列表 / item，含订单数、未完成订单数、充值次数 |
| orders.list / get | 通用条件，可加 userId、stationId、chargerId、createdAtFrom、createdAtTo / id | 只读订单、计费快照、时间、站点 ID/名称、桩号、脱敏用户 |
| recharges.list / get | 通用条件，可加 userId / id | 全局或指定用户充值记录、安全用户摘要 |

列表输入：keyword（最多 64 字）、可选合法 status、page（1–1000000）、pageSize（1–100）、
sort（默认 idAsc；所有列表允许 idAsc / idDesc，订单额外允许 createdAtDesc，电桩额外允许
updatedAtDesc）。时间倒序相同时按 idDesc 打破平局，保证稳定分页，不用 ID 大小代替创建时间。
默认 page=1、pageSize=20；不接受任意 SQL 排序或不适用于该实体的排序。
输出 `{items,total,page,pageSize}`；详情输出 `{item}`。类型、枚举、未知字段都严格校验。
站点 ACTIVE/INACTIVE；桩 AVAILABLE/RESERVED/CHARGING/FAULT/OFFLINE；用户 ACTIVE/FROZEN；
订单 RESERVED/CHARGING/WAITING_PAYMENT/COMPLETED/CANCELLED；充值 SUCCESS/FAILED。

订单时间筛选作用于 createdAt，而非 startedAt / paidAt；范围为 `[createdAtFrom, createdAtTo)`，
两端均可单独提供，同时提供时 From 必须早于 To。格式固定为 UTC
`yyyy-MM-ddTHH:mm:ss.zzzZ`，例如 `2026-09-05T00:00:00.000Z`；页面选择本地日期后先换算成
对应 UTC 边界，不直接上传本地日期或带偏移字符串。站点、电桩、用户、状态、关键词及时间
条件按 AND 组合，items 与 total 使用完全相同的筛选和读事务。

Dashboard 复用现有仓储的 UTC 日/月口径，并在读事务中获取一致统计快照：只汇总
COMPLETED 订单 amountCents，以 paidAt 归属日期，充值不是营收。趋势缺失日期补零。
activeOrders 包括预约、充电中、待支付。在线率=(总桩数-离线数)/总桩数，无设备返回 0；
预约、故障、离线分别计数，不计入空闲。页面应标注 UTC，不得按本地日期误标。

### 运营概览两张摘要表（PR #26 审查补充）

`dashboard.get` 在同一个 SQLite 读事务中返回汇总、趋势和以下两个对象，复用管理列表的
查询与 DTO，不维护另一套 Mock 或缓存：

- `abnormalChargers: {items,total,page:1,pageSize:5}`：只包含 FAULT / OFFLINE 桩，按
  updatedAtDesc、idDesc 排序，最多 5 条；total 是全部异常桩数量，可直接用于摘要角标。
  “查看全部异常”调用 `chargers.list {abnormalOnly:true,sort:"updatedAtDesc",page:1,pageSize:20}`。
  abnormalOnly 必须为 JSON 布尔值；false 或省略不启用异常过滤。
- `latestOrders: {items,total,page:1,pageSize:5}`：所有状态订单按 createdAtDesc、idDesc
  取前 5 条；total 是全量订单数量。包含关联站点、桩号和脱敏手机号，字段与 orders.list/get
  一致。“查看全部订单”调用 `orders.list {sort:"createdAtDesc",page:1,pageSize:20}`。

空数据时 items=[]、total=0，页面显示空态，不填充演示记录。修改桩状态、模拟重启或新增
订单后重新请求 dashboard.get 即得到当前数据库状态；与其他管理列表在无中间写入时一致。

异常定义在本接口版本冻结为**当前状态分类**：exceptionType=FAULT（故障）、OFFLINE（离线）；
其来源仅为 chargers.status。其他状态的 exceptionType=null。离线不推断成硬件故障，也不
返回“过温”“枪通信异常”“模块故障”等数据库没有记录的细分诊断。

数据库当前没有故障事件时间字段，因此不返回或伪造 exceptionAt。updatedAt 是**电桩记录
更新时间**，不是异常发生时间；页面原“异常时间”列应改为“记录更新时间”，或保留原标题
并显示“未记录”，不能把 updatedAt 当故障事件时间。后续需要真实诊断/事件时间时再新增
事件模型和采集来源，不改变本字段含义。

## 管理写操作

所有写操作必填 operationId（1–64 位字母/数字/下划线/短横线，建议 UUID）。除创建外，
必填 id 和详情返回的 expectedUpdatedAt；编辑前保存该值，CONFLICT 后重新查询。

| action | 其余必填参数 | 规则 |
| --- | --- | --- |
| station.create | code、name、address、latitude、longitude、priceCentsPerKwh、chargers | 新站 ACTIVE，1–100 个电桩同事务创建；每桩 code/type/powerWatts |
| station.edit | name、address、latitude、longitude、priceCentsPerKwh | 不改站点编号；已有订单电价快照不变 |
| station.status | status=ACTIVE/INACTIVE | 停站禁止活动预约、预约/充电订单及占用桩；待支付不阻塞停站 |
| user.status | status=ACTIVE/FROZEN | 冻结禁止活动预约及 RESERVED/CHARGING/WAITING_PAYMENT 订单；不阻断用户结算 |
| charger.status | status=FAULT/OFFLINE | 只能操作没有活动预约和充电占用的桩 |
| charger.restart | 无 | AVAILABLE/FAULT/OFFLINE → AVAILABLE；所属站必须启用；返回 simulated=true |

桩重启是受控数据库状态模拟，不向真实硬件发送命令，不自动结束充电或取消预约。
活动充电故障需走单独的安全停机/结算流程，本接口返回 RESOURCE_BUSY，不强行重置。
停站只禁止新的预约，不覆写桩自身故障/离线状态；重新启用站点不自动清除故障。

写事务采用 BEGIN IMMEDIATE，事务内重新鉴权，检查旧版本及业务占用，更新后返回
`{item,idempotent}`（重启额外 simulated）。updatedAt 单调推进，避免同毫秒并发覆盖。
更新和审计任一步失败全部回滚，创建站点时任何桩编号冲突也全部回滚。

operation_logs 中 ADMIN_COMMAND 条目持久化操作指纹与安全结果：同管理员、同 operationId、
相同参数重试返回已保存结果，不再次执行。即使重启后也能去重；同编号不同内容返回 CONFLICT。
重试必须保持原参数（包括 expectedUpdatedAt），不能因超时新建编号。
重放返回历史成功结果，页面应再查询当前数据。日志删除会丢失对应去重能力，不能任意清理。

稳定错误码：INVALID_CREDENTIALS、UNAUTHORIZED、RATE_LIMITED、INVALID_ARGUMENT、
NOT_FOUND、RESOURCE_BUSY、CONFLICT、INVALID_STATE_TRANSITION、DATABASE_ERROR、
TIMEOUT、UNAVAILABLE。页面只依据 code 决策，不解析 message 文本。

## 验证

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DCHARGING_PLATFORM_STRICT_QT_VERSION=ON
cmake --build build --parallel 2
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
```

新增 admin_service、admin_gateway、admin_login 测试覆盖：鉴权/禁用/改凭据/过期、
非法查询参数/脱敏、站点原子创建与编辑、旧版本冲突、持久化并发重试、审计失败回滚、
冻结和充电占用冲突、重启模拟、UTC 营收口径、GUI 不阻塞、请求超时/销毁/覆盖/退出、
真实登录页。其余管理页面接线需按本文契约做 UI 联调，不以原 Mock 按钮提示作为验收。
