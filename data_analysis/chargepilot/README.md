# ChargePilot：AI 智能找桩与错峰激励

基于《智能找桩》实现的第二阶段 **Vue 主展示系统**。不是页面 Mock：预测来自实际训练的模型，订单/排队/支付/积分实际保存到独立 MySQL；但电站、车辆、资金和运营过程均为**模拟演示**，不连接真实充电设备或第三方支付，也不连接一期 Qt。

## 1. 已实现什么

| 模块 | 实现 |
| --- | --- |
| 智能找站 | 五城市25站地图/坐标选起点，逐站 ETA，预计到站空闲数、至少一桩可用概率、条件平均/P90等待、服务成功概率 |
| 可解释推荐 | 可配置六项加权0–100评分，逐项贡献和理由，最近站与推荐站总时间对比，当前订单/排队/在途需求保守修正 |
| 激励 | 前两名差异化积分、区域忙碌/不均衡倍率、推荐资格快照、到站报价锁定、最低电量/时长/有效期及每日预算 |
| 行程闭环 | 选择→模拟导航/到站→排队或预约→叫号确认→充电→目标自动停止或手动停止→待支付→模拟支付→积分到账 |
| 多用户 | 每浏览器独立演示会话、每用户一笔未完成行程、MySQL事务锁防超卖、FIFO、过期释放、2秒同步、后台时钟推进 |
| 模型实验室 | 到站模型TEST与基线、概率校准/服务AUC/P90覆盖，复用PR66未来1/6/24小时负荷模型与曲线 |
| 对照实验 | 同1000名虚拟用户/同种子，最近站与AI+积分分别做容量约束仿真；实际计算指标，保留变差结果 |
| 运营反馈 | 自动记录选择、到站实际空闲数、实际等待、充电和奖励；管理员导出，供后续独立离线训练 |

主链路：

```text
Vue 起点/电量 → FastAPI → ETA → 到站模型 → 解释排序/激励快照
                                      ↓
Vue 行程 ← MySQL 事务状态机 ← 选择/到站/确认/充电/支付
                 ↓
          积分账本 + 反馈导出
```

## 2. 为什么复用 PR66，但还要新模型

PR66 的 `ml/load/` 预测**未来完整小时的 kW 负荷**，共享其过去24小时特征构建器、训练与在线预测器，未复制一套假模型。`load_adapter.py` 加入批次/特征/文件哈希校验和HTTP接入。

到站可能是12分钟后，不能把“某小时结束时的空闲数”当作12分钟后的结果。因此新增 `ml/`：5分钟状态分类分布、服务成功分类、成功开始充电条件下的平均等待与P90回归。使用CPU梯度提升树，不强行添加缺乏必要性/验证的深度网络。训练、验证、测试按时间切分，跨界标签清除，线上线下用同一特征函数。

已有 CLEAN 有5,830,923行、3,888,000条5分钟桩遥测、121,539条有效会话及关联排队/失败请求，足够支持此次模拟系统。**没有使用 ACN-Data 实测记录训练，也没有声称现有模拟数据是真实数据。** ACN官方数据以连接/离开/电量等充电会话为主，不能补出它没有的失败到访、队列和积分标签，因此本次没有为了“真实数据”而虚构这些字段。[ACN字段与访问方式](https://ev.caltech.edu/dataset)

详细防泄漏规则、真实计算的模型指标和限制见 [模型说明](ml/README.md)。

## 3. 一次准备，之后直接启动

环境：Python **3.11/3.12**、Node **24**（满足老师要求≥23）、MySQL **8.4**；不需要GPU。开发/演示推理不需要启动Spark/Hadoop；数据清洗与HDFS仍使用仓库原数据层流程。以下命令除 `npm` 外均在**仓库根目录**执行。

### 3.1 安装、训练

```powershell
python -m venv data_analysis/.venv
# Windows PowerShell
.\data_analysis\.venv\Scripts\Activate.ps1
python -m pip install -r data_analysis/requirements-chargepilot.txt
python -m data_analysis.chargepilot.cli train
python -m data_analysis.chargepilot.cli experiment --users 1000 --seed 42
```

Linux激活命令换成 `source data_analysis/.venv/bin/activate`。训练只读仓库现成 CLEAN，不用重生成数据或重跑Spark。到站模型通常几十秒，PR66完整24个模型需数分钟，实际时间取决于CPU。完整模型只训练一次；`cli train` 检查存在且有效的工件后复用，不覆盖。

`outputs/` 中的模型、缓存、实验结果不进Git，避免115MB负荷模型超过仓库单文件限制，也防止未验证pickle混入源码。另一台电脑应安装同版本依赖后运行训练；或仅复制**自己生成且可信**的完整 `outputs/chargepilot` 和 `outputs/ml_load`。不要加载陌生人提供的 `.joblib/.pkl`。

### 3.2 独立运营 MySQL

用数据库管理员在本地执行（替换示例密码；不要把新表建入统计只读库）：

```sql
CREATE DATABASE chargepilot_demo CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;
CREATE USER 'chargepilot_app'@'127.0.0.1' IDENTIFIED BY '替换为自己的密码';
GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, INDEX
ON chargepilot_demo.* TO 'chargepilot_app'@'127.0.0.1';
```

如果已有同名库，请复用并检查权限或另起新名字，**不要删除旧库**。程序幂等建 `cp_*` 表，保留已有行程与积分；无需向这个库导入上百万行CLEAN。

PowerShell设置当前终端环境（关终端即失效）：

```powershell
$env:CHARGEPILOT_MYSQL_HOST="127.0.0.1"
$env:CHARGEPILOT_MYSQL_PORT="3306"
$env:CHARGEPILOT_MYSQL_DATABASE="chargepilot_demo"
$env:CHARGEPILOT_MYSQL_USER="chargepilot_app"
$env:CHARGEPILOT_MYSQL_PASSWORD="替换为自己的密码"
$env:CHARGEPILOT_ADMIN_TOKEN="替换为至少16位随机管理员令牌"
```

Linux对应 `export CHARGEPILOT_MYSQL_HOST=127.0.0.1` 等。全部变量参考 [.env.example](.env.example)；程序**不会自动读取这个示例文件**。管理员令牌仅在运营控制台手动输入，前端不内置令牌；不设置则管理员接口明确禁用。公共演示昵称不是生产账号认证，请只用于可信局域网/本机，不直接部署到公网。

### 3.3 构建网页并启动

```sh
cd data_analysis/frontend
npm ci
npm run build
cd ../..
python -m data_analysis.chargepilot.cli check
python -m data_analysis.chargepilot.cli serve
```

浏览器打开 **http://127.0.0.1:8000/**；接口文档 **http://127.0.0.1:8000/docs**。开发前端可另开终端 `cd data_analysis/frontend`、`npm run dev`，打开5173端口，Vite代理同机8000。局域网验证时按需 `serve --host 0.0.0.0`，配合防火墙仅开放给组员，不暴露MySQL。

### 3.4 地图和 ETA

不填Key也能展示/演示：OSM地图正常联网时显示真实地理底图（电站位置仍为模拟）；底图网络异常明确切换本地地理示意，起点可点选。默认ETA是“直线距离×1.3、28km/h及2分钟准备”的情景估计，路线是明确标注的演示连线，**不是道路导航**。

有腾讯WebService Key时，在后端进程环境设置 `TENCENT_MAP_API_KEY`，启用签名校验时另设 `TENCENT_MAP_SECRET_KEY`。密钥不进入Vue、不提交Git。服务端调用一对多距离矩阵及路线规划，区分矩阵的秒与路线的分钟、WGS84/GCJ02转换；超时只做一次有界尝试并冷却30秒，不无限重试。腾讯返回的是**当前**交通路线，不能描述成2026年5月历史路况。[腾讯路线接口说明](https://github.com/TencentLBS/tencentmap-webservice-skill/blob/main/references/api-direction.md)

本次没有使用有效外部Key做真实计费接口验收；已验证模拟响应、单位、缓存、降级和坐标转换，现场配置后可补测真实路线。

## 4. 课堂演示流程（约5–8分钟）

1. 首页说明“模拟数据/回放时间”，选择大连→北京，地图与5座对应站点变化。创建昵称；点地图设置起点，输入目标充电量，生成推荐。
2. 打开第一名解释：比较预计到达、空闲概率、P90等待、价格、积分和六项评分；说明与“只找最近”区别。
3. 选择电站生成行程。运营控制台输入令牌，暂停或推进模拟时间至ETA，回到行程确认到站→开始充电。推进10分钟，展示服务端电量/费用和目标自动停止；模拟支付后显示积分与时间轴。重复支付不会重复发积分。
4. 排队扩展：先在控制台把某站背景占用设为容量、释放时间设长；两个独立浏览器/无痕窗口用不同会话选择同站、达到ETA后到站排队。控制台释放背景或等待到期，队首叫号；不确认再推进60秒，下一位自动被叫号。已叫号/预约用户占资源，取消或超时会释放。**不要只开同浏览器两个普通标签，它们共用localStorage会话。**
5. 模型实验室：展示到站模型TEST与基线，选择站点查看PR66实际负荷曲线，解释MAE不是全部领先。展示1000人配对结果和实验假设；运行实验按钮为异步任务，约几十秒后返回真实结果。
6. 运营控制台修改权重/积分，再推荐；旧资格保留当时快照，不被新配置偷偷改变。导出反馈供后续训练。

新库初始回放2026-05-05 08:00（北京时间），默认 **10倍**；每1秒后台推进，前端每2秒同步。推荐5个回放分钟过期，过期应刷新；选中后的行程与奖励资格单独保留。起点/城市改变必须重新生成推荐。

初始背景占用来自一条历史完整快照，随后用明确的模拟释放事件和本次用户操作演进，**并非持续拉取历史整段来覆盖用户占用**。单桩演示功率取站点总功率÷容量的虚拟均分值，不声称还原AC/DC设备异构功率。重启不清空订单、不重置时钟；若运行超出历史预测范围，已有行程仍可处理，推荐会明确拒绝，请暂停回放或用一个新命名的演示数据库开始新场景，保留旧库。

## 5. 实验结果怎么讲，不能怎么讲

固定配方、1000请求、种子42、TEST日期2026-05-06，实际 `capacity-insertion-v2-causal-lease` 结果：

| 指标 | 最近站 | AI＋积分情景 |
| --- | ---: | ---: |
| 成功服务人数 | 688 | 827 |
| 平均等待（失败按120分钟计） | 66.605分钟 | 50.470分钟 |
| 平均行驶 | 7.466分钟 | 11.415分钟 |
| 已完成行程平均总用时 | 61.845分钟 | 59.834分钟 |
| 资源忙碌度标准差 | 0.1363 | 0.1141 |
| 满资源时间占比 | 51.06% | 56.25% |

该情景等待减少，但行驶增加、满资源时间也增加，不能说所有指标都改善。这里是**固定背景优先的容量插入仿真**，不是实地随机试验，也不等同业务FIFO队列：起点/目标电量/80%积分接受率是显式假设，承诺租约固定为ETA+120分钟+充电时长；不读取未来成功/失败作为推荐输入。实验中的忙碌度包含维护/预留/占位，不是纯充电利用率。失败惩罚、配置、请求哈希、模型哈希和所有原始指标一同导出，网页不得只挑有利数字。

## 6. 测试与目录

```sh
python -m unittest data_analysis.tests.test_chargepilot_ml data_analysis.tests.test_chargepilot_experiment data_analysis.tests.test_ml_load_review_fixes -v
# 连接专用测试MySQL，设置RUN_MYSQL_TESTS=1及MYSQL_TEST_HOST/PORT/USER/PASSWORD后：
python -m unittest data_analysis.tests.test_chargepilot_store data_analysis.tests.test_chargepilot_integration -v
cd data_analysis/frontend
npm test
npm run build
```

真实MySQL测试只创建随机 `cp_store_test_*` / `cp_http_test_*` 库，并仅清理自身创建的库；测试账号需创建/删除测试库权限，禁止使用生产数据库执行。CI新增CPU训练/MySQL闭环和Vue构建测试；保留已有第二阶段检查，不恢复一期Qt CI，不改分支保护规则。

2026-09-14 本地验证：64项核心后端测试全部通过（含真实MySQL事务/并发与HTTP闭环）；前端17项测试、类型检查和生产打包通过。数据分析全量测试共327项，其中90项因未启用对应Spark/MySQL等可选环境跳过，其余通过；上述64项核心测试另行开启专用MySQL执行，没有跳过。浏览器实际走通选站→预约→20kWh目标自动停止→22.40元模拟支付→15积分到账，验证五城市切换、390px/1280px布局、PR66负荷曲线及网页发起的1000人异步实验。未代替老师Linux/Hadoop环境验收，也未声称远程CI已通过。

| 文件/目录 | 职责 |
| --- | --- |
| `ml/data.py`、`ml/train.py`、`ml/predict.py` | CLEAN准备、训练与评估、同特征在线推理 |
| `load_adapter.py` | 复用PR66、可信工件及指标绑定、小时负荷预测 |
| `routing.py`、`ranking.py` | ETA/道路或降级路线、解释排序与激励计算 |
| `store.py` | 独立MySQL、鉴权、事务状态机、积分账本、反馈 |
| `app.py`、`cli.py` | `/api/v1/chargepilot`、后台时钟、启动/训练/实验/导出入口 |
| `experiment.py` | 配对仿真、因果隔离、资源容量校验及真实报告 |
| `../frontend/` | Vue3页面、图表、地图与API调用 |

接口说明见 [CONTRACT.md](CONTRACT.md)。运营反馈导出：`python -m data_analysis.chargepilot.cli export-feedback`；不会自动将反馈混入已评估模型。将来新增真实数据、真实地图路况或在线优化应重新评估，不能继承此次模拟准确率。
