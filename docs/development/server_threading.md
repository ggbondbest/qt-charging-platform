# 服务端多线程运行与协作边界

## 实现范围

目标为 Ubuntu 22.04 + Qt 6.2.4、C++17。不修改 JSON 协议、Schema、业务状态机或计费规则；
不引入 PR #19 尚未合并的接口，也不将 PC 管理页的 Mock 宣称为真实业务。

| 所属线程 | 对象与职责 |
|---|---|
| GUI 主线程 | QApplication、MainWindow、ServerRuntime、QThread 对象本身；界面和状态通知 |
| charging-service-worker | 在 QThread::run 内创建的 DatabaseConnection、Repository、Service、RequestDispatcher、ChargingServer、QTcpSocket、ClientSession；事件循环与真实请求 |

选择一个服务线程串行处理多客户端业务，避免多个连接无序修改同一个 SQLite 状态；
该实现解决“慢数据库卡住管理界面”，不保证慢数据库期间其他客户端仍低延迟。
Qt 网络自身为异步事件驱动；不用为了展示线程数而每个客户端创建一个线程。

## 主要文件

- `server/network/server_runtime.h/.cpp`：GUI 门面、工作线程装配、异步状态与关闭。
- `server/app/main.cpp`：原有数据库/服务装配移出 GUI；窗口关闭后等待 stopped 再退出应用。
- `server/pages/main_window.h/.cpp`：仅接收 ServerRuntime 的连接数量，不跨线程访问 ChargingServer。
- `server/network/charging_server.cpp`：显式停止监听并销毁活动 Socket/Session，防止 QObject 析构期间访问已销毁成员。
- `server/network/client_session.cpp`：收到线程停止请求后不继续处理粘包中尚未开始的请求。
- `tests/tst_server_runtime.cpp`：真实 TCP、SQLite、线程生命周期集成测试。

## 生命周期

`Idle → Starting → Running → Stopping → Stopped`；启动失败从 Starting 进入 Stopping/Stopped。

1. start 只允许调用一次，返回 true 代表已发起，不代表监听成功；收到 listening 才可使用端口。
2. 初始化、Schema/演示 Seed、监听均在工作线程完成。失败通过安全错误通知，关闭已建立的资源。
3. stop 是幂等异步操作；Starting 阶段也允许 stop。停止后忽略过期的 ready/连接数量通知。
4. 正在执行的事务自然结束；未开始请求可被丢弃。退出不是业务取消，不会自动停止充电或修改订单。
5. 工作线程退出事件循环后，先释放 Socket/Session，再释放 Dispatcher/Service/Repository，最后关闭数据库。
6. stopped 表示工作对象和数据库已释放；需要重启时创建新的 ServerRuntime，不复用旧实例。
7. 门面析构提供 requestInterruption + quit + wait 兜底，绝不销毁运行中的 QThread 或强制 terminate。
   正常窗口退出走异步 stopped 流程；如果业务卡在系统 I/O 中，析构仍需等待，不承诺硬实时关闭。

服务停止时，某个已提交的写操作可能来不及把响应送达客户端。保留既有充值流水号重试、
支付幂等和登录后查询机制；不要因连接断开自动重发所有写操作，也不要用重建订单“修复”超时。

## 后续组员必须遵守

- ServerRuntime 公共方法及状态读取只在 GUI/拥有者线程调用；不向外暴露服务层或数据库指针。
- 管理页要接真实业务时，增加明确的数据请求/结果消息，通过 queued connection 送入工作线程对象；
  不把业务槽放到 QThread 对象上，也不能直接调用工作线程的 Service 或在 GUI 里写 SQL。
- queued 消息传递值类型 DTO；自定义类型在 Qt 6.2.4 中先完成元类型声明/注册。
- 若以后增加第二个数据库工作线程，必须在线程内部新建独立连接，不能复制本线程的 QSqlDatabase 使用。
- 不调用较新 Qt 的 QSqlDatabase::moveToThread；本实现从创建开始就保证线程归属。
- 单线程网络测试夹具仍可注入原 RequestDispatcher；它们测试协议/业务，不代表生产运行在线程内的证据。

## 自动化验证

在已配置 Qt 6.2.4 的 Ubuntu 环境执行：

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DCHARGING_PLATFORM_STRICT_QT_VERSION=ON
cmake --build build --parallel 2
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
ctest --test-dir build -R '^server_runtime$' --repeat until-fail:20 --output-on-failure
```

新测试覆盖：主线程不处理事件时完成真实登录与完整充电流程；半包/粘包登录顺序；
两个客户端身份隔离及抢桩；SQL 写锁等待期间主线程定时器持续执行；关闭活动连接；
重新启动后数据持久化；数据库/端口启动失败；启动时立即停止及析构；数据库连接名无泄漏。

合并前以 Ubuntu / Qt 6.2.4 的 PR CI 为准；较新 Qt 的本地测试只是补充证据。
手工验收仍应检查窗口操作、关闭程序、数据库重开，以及两台客户端的真实业务表现。
