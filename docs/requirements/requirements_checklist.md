# 需求与验收 Checklist

勾选规则：只有在 Ubuntu 22.04 + Qt 6.2.4 中实际验证并留下证据后，功能项才能从 `[ ]` 改为 `[x]`。页面占位、静态假数据或只在开发者电脑运行不算完成。

严格构建与测试证据：[手机号登录最小闭环 PR #3](https://github.com/ggbondbest/qt-charging-platform/pull/3)。

## 1. 范围基线

- [x] 首阶段范围锁定为 Qt 用户端、Qt PC Server/管理端、SQLite；
- [x] Web 大数据大屏移出首阶段；
- [x] 机器学习智能分析移出首阶段；
- [x] 技术基线锁定为 Ubuntu 22.04、Qt 6.2.4、C++17、CMake、SQLite、Socket、多线程；
- [ ] 五人确认范围和接口冻结记录；
- [ ] GitHub 建立 `master <- develop <- feature/*` 流程并保护 `master`；
- [ ] 每个跨模块 PR 至少一名非作者 review。

## 2. 阶段 0/1 准备

- [x] 公共 target `charging_common` 已定义；
- [x] 数据库资源 target `charging_database_resources` 已定义；
- [x] 文件、C++、JSON、SQL、Git 命名规范已写入架构文档；
- [x] User/Admin/Station/Charger/Reservation/Order/RechargeRecord/OperationLog 公共模型已定义；
- [x] 状态 enum 与唯一大写字符串映射已定义；
- [x] 金额分、功率瓦、电量瓦时、时长秒、UTC 时间候选单位已定义；
- [x] Socket JSON candidate-v1 envelope 已定义；
- [x] TCP 4 字节大端长度 framing 和半包/粘包 decoder 已实现；
- [x] schema v1、seed、数据字典已建立；
- [x] schema/seed 已配置为 Qt resource；
- [x] common 单元测试在 Qt 6.2.4 通过；
- [x] schema/seed 自动验证进入 CTest/CI；
- [x] Ubuntu 22.04 + Qt 6.2.4 clean configure/build 通过。

## 3. 最小登录闭环（阶段 2 门槛）

- [x] Client 登录页校验 `^1[0-9]{10}$`；
- [x] Client 异步连接 Server，并发送 frame 化 `USER_LOGIN`；
- [x] Server 能处理头/正文半包和多帧粘包；
- [x] Dispatcher 校验 v1 envelope 并路由到 UserService；
- [x] UserRepository 按手机号查询 SQLite；
- [x] 已有活动用户返回同一用户；
- [x] 新手机号只创建一次，昵称为 `用户` + 后四位，余额为 0；
- [x] 冻结用户返回 `USER_FROZEN`；
- [x] Response 回显相同 type/requestId；
- [ ] Client 按 requestId 匹配结果并进入首页；
- [ ] 重连后同手机号返回原 user ID；
- [ ] 两个并发 Client 首次登录同手机号，数据库最终只有一行；
- [ ] Server 重启后数据仍存在；
- [x] Client -> Socket -> Server -> Service -> Repository -> SQLite -> Client 集成测试通过。

最小闭环和严格 CI 已通过；进入首页、重连、并发首次登录和 Server 重启持久化仍按上方
未勾选项继续补齐。成员可在各自目录基于候选接口并行开发，但公共契约冻结仍需五人确认，
且不得各自重复创建网络层或数据库基础层。

## 4. 用户端基础功能

### 4.1 登录与个人资料

- [x] 11 位手机号免密登录；
- [x] 新手机号自动注册；
- [ ] 默认灰色头像；
- [ ] 展示昵称和钱包余额；
- [ ] 本地选择头像并通过受控资源接口保存；
- [ ] 修改昵称并持久化；
- [ ] 正金额模拟充值；
- [ ] 余额和充值流水同事务更新；
- [ ] 查询充值记录；
- [ ] 冻结用户不能继续业务操作。

### 4.2 附近电站与导航

- [ ] 下拉选择区域；
- [ ] 手动输入地址；
- [ ] 地址通过腾讯地图 Web API 转换为经纬度；
- [ ] 站点按距离由近及远排序；
- [ ] 电站卡片展示站名、电价、总桩/空闲桩、距离；
- [ ] 电站详情展示电桩编号、类型、状态、功率；
- [ ] QWebEngineView 加载腾讯地图路线页；
- [ ] 起点和终点正确传入；
- [ ] 驾车/步行方式可选择；
- [ ] 地图/API 失败有可恢复提示；
- [ ] API key 不硬编码进公开仓库。

### 4.3 预约—充电—计费—结算

- [ ] 进入充电前检查未完成订单；
- [ ] 有未结订单时提示并跳转结算；
- [ ] 只能预约 `AVAILABLE` 电桩；
- [ ] 同一用户/电桩不能重复活动预约；
- [ ] 取消预约同步释放订单和电桩；
- [ ] 预约超时由 Server 权威处理；
- [ ] 有效预约才能开始充电；
- [ ] 预约、订单、电桩状态在一个事务中进入充电；
- [ ] 展示实时功率、电量、时长、金额；
- [ ] 断线后可恢复查询当前充电状态；
- [ ] 停止充电生成待支付订单并释放电桩；
- [ ] 金额按订单电价快照和整数规则计算；
- [ ] 余额不足不完成订单、不扣成负数；
- [ ] 支付扣款与订单完成同事务；
- [ ] 重复停止/重复支付不会重复更新或扣款；
- [ ] 历史订单可查询并查看详情；
- [ ] 后台能实时/刷新看到订单和桩状态变化。

## 5. PC Server / 管理端基础功能

### 5.1 管理员与 Dashboard

- [ ] `admin / 123456` 可登录演示库；
- [ ] 数据库不存管理员明文密码；
- [ ] 错误密码和禁用管理员被拒绝；
- [ ] 展示今日、本月、总营收；
- [ ] QChart 展示近 7 日营收；
- [ ] QChart 展示近 30 日营收；
- [ ] 时间维度切换正确；
- [ ] 营收只统计已完成订单；
- [ ] 表格展示在用、闲置、故障数量及百分比。

### 5.2 电桩管理

- [ ] 列表展示编号、所属站、快/慢充、功率、状态；
- [ ] 展示累计充电次数和累计时长；
- [ ] 状态筛选/刷新；
- [ ] 远程重启为受控模拟动作；
- [ ] 对活动订单关联电桩的异常操作遵循状态机；
- [ ] 管理操作写入 operation_logs。

### 5.3 电站管理

- [ ] 列表展示 ID/编号、站名、地址、经纬度；
- [ ] 总桩数和在线率由 chargers 聚合；
- [ ] 点击站点查看全部电桩实时状态；
- [ ] 新增站点校验名称、地址、坐标、电价；
- [ ] 新增站点/电桩失败时事务回滚；
- [ ] 新增操作写入 operation_logs。

### 5.4 用户与订单管理

- [ ] 展示用户 ID、手机号、昵称、余额、注册时间和状态；
- [ ] 手机号模糊搜索；
- [ ] 分页/排序不在 UI 线程执行长查询；
- [ ] 冻结用户；
- [ ] 解冻用户；
- [ ] 冻结/解冻写入 operation_logs；
- [ ] 管理员可查看订单状态和关键计量字段。

## 6. 数据库与一致性

- [x] users 表；
- [x] admins 表；
- [x] stations 表；
- [x] chargers 表；
- [x] reservations 表；
- [x] orders 表；
- [x] recharge_records 表；
- [x] operation_logs 表；
- [x] 外键、CHECK、查询索引、active partial unique index；
- [x] seed 管理员、演示用户、站、桩和充值流水；
- [ ] 应用初始化按 `PRAGMA user_version` 执行 schema/迁移；
- [ ] 每线程/工作线程使用自己的 QSqlDatabase connection；
- [ ] 所有动态 SQL 使用 prepared query + bind；
- [ ] Repository 不向 Client 泄露原始数据库错误；
- [ ] 预约事务失败回滚；
- [ ] 开始/停止充电事务失败回滚；
- [ ] 余额扣除/充值事务失败回滚；
- [ ] DB 锁定、磁盘错误和损坏有完整错误处理；
- [ ] `foreign_key_check` 无行；
- [ ] `integrity_check` 为 `ok`；
- [ ] schema 版本升级有迁移测试。

## 7. Socket、多线程与错误处理

- [x] v1 最大 payload 为 1 MiB；
- [x] requestId 关联规则；
- [x] 稳定协议和业务错误码第一版；
- [ ] Socket 连接、断开、超时、重连均有 UI 状态；
- [ ] 无阻塞 `waitFor*` 调用卡住 GUI；
- [ ] QTcpSocket 只在所属线程读写；
- [ ] SQLite connection 不跨线程；
- [ ] 工作线程退出时安全关闭 DB 和对象；
- [ ] malformed JSON 不导致 Server 崩溃；
- [ ] 超大/零长度 frame 导致连接安全关闭；
- [ ] 未知动作返回 `UNKNOWN_REQUEST_TYPE`；
- [ ] 未授权动作返回 `UNAUTHORIZED`；
- [ ] 错误响应和日志不包含密码、hash、salt、SQL 或本地路径；
- [ ] 多 Client 同时连接测试；
- [ ] Server 主窗口关闭时连接和线程可控退出。

## 8. UI 与可用性

- [ ] 用户端统一新能源汽车 App 风格；
- [ ] PC 端统一 Dashboard 布局：侧栏、顶栏、QStackedWidget；
- [ ] 颜色、字体、字号、间距、圆角、图标 token 已冻结；
- [ ] Button/Input/Table/Dialog/Toast/状态标签统一；
- [ ] QSS/SVG 资源集中管理；
- [ ] 页面具备 loading、empty、error、disabled 状态；
- [ ] 不大量保留 Qt 默认控件样式；
- [ ] Ubuntu 字体缺失时不乱码、不溢出；
- [ ] 关键页面在目标分辨率截图评审。

## 9. Qt 6.2.4 兼容与交付

- [x] 公共代码限制 C++17；
- [x] 公共实现未使用较新 Qt 异步 API；
- [ ] Client/Server 全部 API 经 Qt 6.2.4 编译验证；
- [ ] Ubuntu 安装说明包含 Qt Base、Sqlite driver、Charts、WebEngine 的准确依赖；
- [ ] clean clone 后无需本机绝对路径；
- [ ] 删除 build 后按 README 可从零配置；
- [ ] 首次运行可创建数据库；
- [ ] Server 先启动、Client 后启动可完成演示；
- [ ] 演示前准备可重复 seed/重置方案；
- [ ] 最终至少预留 1～2 天只做 Linux 验收和修复；
- [ ] 发布版本在 `master` 打 tag，并保存构建/测试记录。

## 10. 测试最低集合

### 正常路径

- [x] 登录已有用户；
- [x] 自动注册用户；
- [ ] 查询站点和桩；
- [ ] 预约、开始、状态刷新、停止、支付；
- [ ] 充值；
- [ ] 管理员登录；
- [ ] 新增站点；
- [ ] 冻结/解冻用户；
- [ ] Dashboard 收入统计。

### 异常路径

- [x] 非法手机号；
- [x] 冻结用户；
- [ ] 错误管理员密码；
- [ ] 重复预约；
- [ ] 预约已占用/故障/离线电桩；
- [ ] 无预约开始充电；
- [ ] 充电中再次开始；
- [ ] 重复停止；
- [ ] 余额不足；
- [ ] 重复结算；
- [ ] Socket 半包/粘包/断开/超时；
- [x] 非法 JSON/错误版本/超大 frame；
- [ ] 数据库锁定和查询失败；
- [ ] 业务事务中途失败后的状态一致性。

## 11. 后续加分项（基础功能 100% 后）

- [ ] 15 分钟预约倒计时和服务端自动释放；
- [ ] 更真实的实时充电模拟；
- [ ] 峰谷电价和计费明细；
- [ ] 收藏站点/最近使用；
- [ ] 充电异常中断恢复；
- [ ] 高级搜索、筛选、排序；
- [ ] 深浅主题、动画和更完整通知；
- [ ] 更丰富运营统计；
- [ ] 评估 Web 大屏；
- [ ] 评估机器学习模块。
