# 五市实训示范数据

`city_demo_seed.sql` 提供大连、沈阳、北京、上海、深圳各 5 座电站，每站 3 根桩（2 快充、1 慢充），总计 **25 站、75 桩**。

这些是实训用的**模拟运营数据**，不是经核实的商业电站目录。名称明确标为“示范”；坐标只表示所在城市/片区的大概位置，用来演示城市地图、选站、选桩、预约和导航。不得把它们当作真实可用的充电资源或现场导航目的地。

## 给已有数据库补齐

先关闭运行中的服务端，保留当前数据库并做好备份，再用原来的数据库路径启动：

```bash
./build/server/charging-server --database /你的原数据库路径/charging-platform.sqlite3 --demo-seed
```

也可以在 Qt Creator 的服务端“命令行参数”中加入 `--demo-seed`，`--database` 仍然填写原路径。**不要删库，不要换成临时空库**。升级成功后可保留或去掉 `--demo-seed`；去掉参数不会删除已经加入的站点。

服务端启动工作线程执行：基础 Schema/Seed → `DatabaseConnection::applyCityDemoSeed()` → 启动 TCP 监听。新增目录只在显式 `--demo-seed` 模式下加载；普通非演示数据库不会自动加入示范站。

## 数据安全约定

- 整批补齐在一个 `BEGIN IMMEDIATE` 事务内完成，失败回滚本批新增行。
- 根据唯一 `code` 执行 `INSERT OR IGNORE`，ID 由数据库生成，不抢占固定数字 ID。
- 已有站名、地址、价格、启停状态，以及电桩状态和充电次数不覆盖。
- 用户、余额、预约、订单、支付和充值数据不由城市目录修改。
- 原来 3 座大连站及 7 根桩保留；仅补齐 2 根桩和其余 22 座示范站。
- 重复启动不重复插入；如果手动删除了某个示范 code，后续 `--demo-seed` 会补回。已存在但经过管理员编辑的条目不会恢复原值。
- 管理员若把示范站地址改为其他城市，客户端按新地址归属展示，不强制修正管理员编辑；因此已有编辑后的库不保证仍严格“每市恰好 5 站”。

`seed.sql` 继续保留 3 站 / 7 桩的最小测试夹具，避免影响既有仓储测试的统计口径。完整运行数据由 `--demo-seed` 的服务启动入口追加；直接调用 `DatabaseConnection::open(path, true)` 仍只载入最小夹具。

## 回归检查

```bash
bash scripts/verify_database.sh
bash scripts/verify_city_demo_data.sh
ctest --test-dir build -R city_demo_seed --output-on-failure
```

检查脚本只使用新建临时测试数据库，不修改当前演示数据库。独立 Qt 测试还覆盖已编辑数据库保留、非连续 ID、注入失败回滚、服务端启动参数和重启幂等。
