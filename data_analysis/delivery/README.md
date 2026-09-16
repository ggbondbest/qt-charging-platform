# 第二阶段统一交付：充能智析

一个 Vue 网页、一个 FastAPI 服务入口。统计和演示业务使用两个隔离的 MySQL 库。
环境：Python **3.11/3.12**、Node **24**、MySQL **8.4**；CPU即可，不需要GPU。
全部电站、用户、充电行为和支付都是**模拟演示**。模型实际训练、实际推理，但不代表真实现场验证。
最终范围、按顺序复现的检查命令和四分钟验收路线见 [最终交付与验收](../docs/final_delivery.md)。

## 1. 整合后的结构

```text
生成数据 → 脏数据注入 → PySpark清洗/统计 → CLEAN + 发布包 → HDFS验收
                                             ├→ MySQL只读统计库 → 运营大屏
                                             └→ CPU训练 → 模型注册 → 智能分析
Vue起点 → 智能找桩接口 → ETA + 分钟到站/等待模型 + 小时负荷均衡 → 推荐
Vue推荐卡片 → 查看路线；独立MySQL业务库存储访问会话、推荐记录和回放时钟
```

| 页面 | 可展示内容 |
| --- | --- |
| 运营总览 | 城市/日期筛选；电量、收入、用户、利用率、资源状态、队列、维修、站型/地区对比、清洗质量；地图和实际图表 |
| 智能找站 | 五城市25站，点选起点、目标电量、ETA、到站可用概率、条件等待/P90、服务成功率、评分解释、积分标签与路线预览 |
| 智能分析 | 1/6/24小时负荷与空闲桩预测、用户流失风险、会话异常复核、实际TEST指标 |
| AI运营参谋 | 全站右下角人物聊天；在线模型检索聚合与项目知识后生成带引用回答，保留本地统计问答 |
| 模拟控制台 | 管理员令牌验证、回放时钟、最近站/智能推荐配对实验；不修改只读统计批次 |

运营总览新增「时空与效率」「用户与服务」两个多维分析工作区：时段热力图、站点效率气泡、
气象关联、指标相关矩阵、补能行为分布、具体服务流失原因、站型×小时瓶颈联动和四层用户画像。
计算口径、复现命令和答辩顺序见 [多维分析说明](../docs/advanced_analytics.md)。
七表完整成果可[追加安装至当前匹配的统计 MySQL 库](../docs/mysql_setup.md#追加七表高级统计)，不覆盖原表或模型。
已安装时优先读取数据库副本，仅整个扩展未安装时读取独立 `datasets/advanced_analytics_v2` 包；半安装、损坏或错批均报错。
另保留 `datasets/cleaning_challenge_v1` 的20类质量挑战与正式清洗器实际处理证据，
见 [清洗挑战说明](../docs/advanced-cleaning.md)。挑战集不作为新的运营数据或模型训练输入。

`delivery/` 负责统一启动和模型聚合；`backend/` 负责统计查询；`chargepilot/` 负责找桩/业务闭环；
`ml/load`、`ml/availability`、`ml/insights.py` 负责各模型，`ml/advisor` 负责已有聚合/报告与项目知识的检索和回答；
`frontend/` 是唯一网页源代码。
原 Qt 客户端、TCP和SQLite不改，也不嵌入第二套网页。

## 2. 首次准备

以下Python命令在**仓库根目录**运行。Windows PowerShell：

```powershell
python -m venv data_analysis/.venv
.\data_analysis\.venv\Scripts\Activate.ps1
python -m pip install -r data_analysis/requirements-delivery.txt
python -m data_analysis.delivery.cli train
```

Linux激活方式：`source data_analysis/.venv/bin/activate`，其余Python/npm命令相同。
训练只读现成180天CLEAN，不必重跑Spark。需要数分钟至十几分钟，取决于CPU；重复运行会验证并复用已有完整模型。
中断/损坏的训练目录需显式检查或换新路径，不自动覆盖已有模型证据。
统一依赖包含PyArrow；不要再安装旧PR的Python3.13/NumPy2.4草稿环境。

产物目录：`outputs/chargepilot`、`outputs/ml_load`、`outputs/ml_availability_delivery_base`、
`outputs/ml_availability_delivery`、`outputs/ml_insights_delivery`。这些文件忽略Git，不下载不明来源pickle；
新电脑应重新训练，也可复制**自己生成且可信、依赖版本相同**的完整产物目录。摘要不等于数字签名。

### 2.1 发布只读统计库

按 [MySQL说明](../docs/mysql_setup.md) 安装MySQL并配置有新库创建权限的**导入账号**。
例如先在当前终端设置 `ANALYTICS_MYSQL_HOST/PORT/USER/PASSWORD/DATABASE`，其中DATABASE用尚不存在的新库名 `charging_analytics_delivery`：

```powershell
$env:ANALYTICS_MYSQL_HOST="127.0.0.1"
$env:ANALYTICS_MYSQL_PORT="3306"
$env:ANALYTICS_MYSQL_DATABASE="charging_analytics_delivery"
$env:ANALYTICS_MYSQL_USER="你的导入账号"
$env:ANALYTICS_MYSQL_PASSWORD="你的导入密码"
python -m data_analysis.publishing.mysql_publish --input data_analysis/datasets/analytics_full_180d_v1 --report data_analysis/outputs/mysql-delivery-publication.json
```

发布器创建新库并校验哈希、行数、外键/城市总量；不会覆盖旧库，未来目标标签不进入查询库。
随后由数据库管理员创建只读账号（示例密码自行替换）：

```sql
CREATE USER 'analytics_reader'@'127.0.0.1' IDENTIFIED BY '替换为自己的只读密码';
GRANT SELECT ON `charging\_analytics\_delivery`.* TO 'analytics_reader'@'127.0.0.1';
CREATE DATABASE chargepilot_demo CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;
CREATE USER 'chargepilot_app'@'127.0.0.1' IDENTIFIED BY '替换为自己的业务密码';
GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, INDEX, ALTER
ON `chargepilot\_demo`.* TO 'chargepilot_app'@'127.0.0.1';
```

已有同名账号/库时复用或另起新名称，**不要删除原数据库**。业务库幂等建立 `cp_*` 表，保留已有订单与积分。

### 2.2 配置服务、构建、运行

服务终端使用只读统计账号和独立业务账号，不再使用导入账号：

```powershell
$env:ANALYTICS_MYSQL_USER="analytics_reader"
$env:ANALYTICS_MYSQL_PASSWORD="替换为自己的只读密码"
$env:CHARGEPILOT_MYSQL_HOST="127.0.0.1"
$env:CHARGEPILOT_MYSQL_PORT="3306"
$env:CHARGEPILOT_MYSQL_DATABASE="chargepilot_demo"
$env:CHARGEPILOT_MYSQL_USER="chargepilot_app"
$env:CHARGEPILOT_MYSQL_PASSWORD="替换为自己的业务密码"
$env:CHARGEPILOT_ADMIN_TOKEN="替换为至少16位随机管理员令牌"
cd data_analysis/frontend
npm ci
npm test
npm run build
cd ../..
python -m data_analysis.delivery.cli check
python -m data_analysis.delivery.cli serve
```

保留上节设置的 `ANALYTICS_MYSQL_HOST/PORT/DATABASE`。Linux对应 `export 变量名=值`。
数据库与业务配置从进程环境读取，不会自动加载 `.env.example`；参谋另支持下面的专用 `.env.local`。不要将密码提交Git。
打开 **http://127.0.0.1:8000/**。统计/智能分析文档在 `/docs`，找桩子系统文档在 `/api/v1/chargepilot/docs`。
前端开发可另开 `npm run dev`，5173通过Vite代理到8000；完整展示无需第二个前端服务。
完整交付只用 `delivery.cli serve`；旧 `backend.app` 和 `chargepilot.cli serve` 是独立子系统调试入口。

AI参谋无需另开服务端口或训练。在线模式由模型规划受控检索，再用当前发布聚合、模型报告和项目知识生成正文及引用；
“你好”等在线问候同样由模型生成。未配置Key时可用本地统计问答；有效配置存在时网页默认在线，主动发送授权本次外发，不再逐次勾选同意框。
首次把 `data_analysis/ml/advisor/.env.example` 复制为同目录 `.env.local`，补齐自己的AIPing Key后重启 `delivery.cli serve`。
组员已有可运行环境时，更新代码并重新构建 Vue 后，只需配置自己的 Key；不需要额外启动参谋服务、训练参谋或创建向量库。
新电脑仍须先完成本节的依赖、模型、MySQL 与前端准备。七表扩展未安装时使用仓库内已校验的数据包，不以额外入库作为在线聊天的前提。
模板预设 `https://aiping.cn/api/v1` 与 `DeepSeek-V4.1-Flash`；文件自动加载，进程 `ML_ADVISOR_*` 配置优先，
包括进程中的 `ML_ADVISOR_ONLINE_ENABLED=0` 覆盖文件的 `1`。GET的 `onlineAvailable` 只代表配置格式有效，不代表真实调用成功。
步骤、环境文件覆盖方式和错误处理见 [AI参谋配置与数据边界](../ml/advisor/README.md)。
每次在线请求会发送问题、同范围有限历史、页面范围、聚合和知识片段；不检索或外发原始业务行、用户/会话明细与完整工件。
完整七表聚合留在本地；按问题选取最多48项成组分析证据，加模型报告后最多64项，从16段口径/方法知识中BM25检索最多4段，不全量发送。
知识先保留本题实际匹配，历史只补空位；旧引用标记在模型上下文中移除，历史数值不代替当前范围的新证据。
这些聚合仍来自模拟业务批次，模型解读须核对数值、单位和来源，不保证完全消除幻觉。Key只留在后端，基础无Key验收选择本地模式。
当前配置环境已实测AIPing `DeepSeek-V4.1-Flash` 的规划、问候及MySQL聚合RAG回答，并核对指标引用。
生成时提供本次允许的引用ID；安全解析器处理完整代码块和字符串内裸换行等格式差异，不猜数据或补写不完整JSON。
精确完整ID的中文/组引用可调整呈现，UI引用列表从规范化后正文实际ID生成，不靠模型的冗余数组补造正文来源；未知或畸形引用仍拒绝。
规划、生成各最多两次尝试；格式/字段/截断/引用问题在各阶段共享一次纠错，通常两次调用、总计最多四次，共享原60秒预算，单次最多45秒。
网络、认证、限流、内容过滤/工具调用及外层协议或超大响应不自动重试；错误日志仅有安全分类元数据，不含问答或密钥。
前端“重试发送”主动重发原问题、原范围及原历史，保留聊天和新草稿；变更范围后旧重试失效，不用重新载入面板代替重试。
有限纠错不能绝对保证外部API持续可用；实际连通和引用匹配也不保证模型全部语义判断正确，仍需逐项复核。

这是教学局域网演示，不是公网生产服务：统计没有生产账号体系；体验昵称不等于真实用户认证。
默认只绑定127.0.0.1，不要公开MySQL和管理员令牌。

## 3. 模型怎样复用

- 智能找桩只请求一次 `/api/v1/chargepilot/recommendations`。内部以ETA选择分钟模型预测窗口，再使用PR66负荷预测辅助均衡项。
- 均衡项总权重默认5%；就绪时取当前占用率与预测功率/额定功率各半。负荷缺失则显式提示仅用占用率，不编造预测，也不阻断基础找桩。
- PR70目标是**某小时末hh:55的空闲桩数**，不是12分钟后到站库存，故作为运营曲线独立展示。区间和无空桩概率来自真实模型输出，不画人为±百分比。
- PR67历史偏好模型只模仿已有用户的历史选站；体验账号没有这种历史，不强行合并到在线评分。代码保留为离线研究，避免重复“找桩”入口。
- 流失和异常分析独立接入同一网页，默认展示留出TEST样本。风险分数不是校准概率；异常为结束会话复核，不是实时电池保护。
- AI参谋在线从五个受控主题中检索已有聚合/模型报告，并以本地BM25检索项目知识，生成带引用的解读；
  不预测新的故障概率，不重训模型，不自动改配置或安排维修。引用可复核，但不等于模型每项推断都已验证。
- 配对仿真为固定“最近站 vs 到站模型+积分”基线实验，**不包含新增小时负荷均衡项**，不能用旧仿真分数声称整合后完整策略收益。

数据都是模拟。界面指标读取当前本地训练报告，旧研究报告不作为当前部署成绩。流失窗口/分转元/跨界完成时间、异常训练参照和模型工件校验均已修复；异常对部分故障类型召回低会如实显示。

## 4. 公共接口

| 接口 | 含义 |
| --- | --- |
| `GET /api/v1/datasets`、`/cities`、`/stations`、`/dashboard/*` | 同批次真实统计查询 |
| `GET /api/v1/intelligence/models` | 实际模型就绪状态、注册ID/跨度、TEST指标、建议历史起点 |
| `POST /api/v1/intelligence/forecast` | `target=load/availability`，同批次、站点、UTC整点、跨度、注册modelId；含真实区间等扩展字段 |
| `POST /api/v1/predict/load`、`/availability` | 保留原严格预测响应结构，不附加未知字段 |
| `GET /api/v1/intelligence/insights/churn`、`/anomalies` | TEST样本风险/异常列表，limit1–100 |
| `GET /api/v1/intelligence/insights/churn/{userId}`、`/anomalies/{sessionId}` | 特征、解释、评分语义和来源 |
| `GET /api/v1/intelligence/advisor` | 配置有效时建议默认在线，否则离线；返回示例、配置可用状态和外发说明，不调用供应商或返回密钥 |
| `POST /api/v1/intelligence/advisor` | 同源统计/RAG问答，返回正文、范围、聚合、知识及引用；在线须 `mode=online, consent=true`，网页主动发送时设置授权字段；省略mode仍为离线 |
| `/api/v1/chargepilot/*` | [原业务契约](../chargepilot/CONTRACT.md)，身份/行程/队列/模拟支付/管理 |

统计/智能分析采用原 `code/message/data/meta` 包装；请求和响应绑定datasetId及publishedBatchId。
前端切筛选会取消旧请求，错批409清空旧结果；模型未就绪503不返回假值。
统计日期是上海业务日，endDate不含当天；金额分÷100为元，Wh÷1000为kWh，比例×100才是百分数。
站点快照为**批次末尾快照**，不会伪称筛选日期的实时状态。

## 5. 地图与演示流程

OSM联网底图保留署名；不可达时明确显示可交互地理示意。起点红色、站点标记可点选。
不填Key也可演示；默认ETA是距离/速度情景估计，不冒充真实道路导航。
腾讯真实路线可在后端设置 `TENCENT_MAP_API_KEY` 和（启用签名时）`TENCENT_MAP_SECRET_KEY`，不送入前端。
外部路况是当前时间，运营数据是历史模拟回放，两者不能宣称同日实测。

四分钟展示顺序见 [验收路线](../docs/final_delivery.md#四分钟验收路线)。需要完整检查时：

1. 数据清单和清洗质量：展示23类数据、清洗前后、隔离原因；Linux按原HDFS文档验证真实上传与读取。
2. 运营总览：全域→大连→北京，变更日期，展示地图/关键指标和不同图表同步变化。
3. 智能分析：选择站点/历史整点，分别预测1/6/24小时负荷和空闲桩，说明kW与桩数不同；展示TEST与基线。
4. 输入模拟用户编号、选择异常会话，展示风险/特征/解释；说明风险非概率、低召回局限。
5. 点击右下角人物展开AI参谋，无Key基础验收选择本地统计问答并核对来源；已配置Key时另验在线问候、追问和聚合/知识引用。缺证据应明确提示。
6. 智能找站：设置出发点、输入目标电量，展示推荐理由、积分标签及负荷辅助说明。
7. 点击推荐电站的“查看路线”，展示红色起点与目的站；页面不创建充电行程。
8. 控制台运行配对实验，自动跳转“最近站 vs. 智能推荐”查看实际结果；需要时调整回放时钟，暂停便于讲解。

统计数据日期为2025-12-01至2026-05-29；默认预测/回放为2026-05月，不使用今天日期制造不存在的历史。
HDFS和有效腾讯Key仍需在实际验收环境独立验证；网页/CPU训练不依赖Hadoop运行。

## 6. 验证与精简范围

CI包括Python静态检查、生成器/交付证据双平台检查、API/契约双平台检查、五套真实Spark测试、
统一CPU训练+MySQL真实集成，以及Vue类型/构建/回归测试。高级分析、清洗挑战和AI参谋均有对应检查，
具体模块见 [CI覆盖表](../docs/final_delivery.md#验证与ci覆盖)。AI参谋的在线协议使用测试传输，不需要真实Key或付费调用。
模型测试实际先训练再跑，不以“缺模型跳过”代替验收；旧研究产物专属测试仍可能明确跳过。
移除了PR72未使用的静态Mock图表、重复主题/入口，以及异常v2/v3/v4并列执行脚本；历史代码可从Git恢复。
没有删除原数据集、数据处理链、一期代码或已有用户数据库。
当前网页已移除行程/充电/支付操作、资源占用表、排队场景构建、推荐策略配置和反馈导出；后端兼容接口与历史业务记录保留。
PR #74未来七天报修预测、PR #78扩容规划不纳入本次交付，分支保留为研究记录；原因见
[最终范围](../docs/final_delivery.md#交付范围)。不为增加模型数量而增加页面或运行依赖。
