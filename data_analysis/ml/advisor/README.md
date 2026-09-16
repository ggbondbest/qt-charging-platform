# AI运营参谋：在线 RAG 与本地统计问答

参谋随 `python -m data_analysis.delivery.cli serve` 运行，使用同源
`GET/POST /api/v1/intelligence/advisor`。在线模式由模型理解问题、规划检索，再结合当前发布聚合
和项目知识生成带引用的回答；问候也由在线模型生成。参谋复用统一服务，无需额外训练任务。

有效在线配置存在时，网页默认选择在线模式；未配置时默认选择无需 Key 的本地统计问答。
离线模式继续使用确定性规则和程序生成的统计说明，适用于运营概况、瓶颈、站点比较、补能行为、
模型说明和历史异常复核。离线模式不是通用聊天模型。

## 先接好 AIPing

以下步骤在仓库根目录、已有统一服务和数据库配置的环境中执行。

1. 首次配置时，将模板复制为参谋专用的本地文件。已有 `.env.local` 时直接编辑，保留原配置。

   ```bash
   cp -n data_analysis/ml/advisor/.env.example data_analysis/ml/advisor/.env.local
   ```

   Windows PowerShell 首次复制可用：

   ```powershell
   Copy-Item data_analysis/ml/advisor/.env.example data_analysis/ml/advisor/.env.local
   ```

2. 在 `data_analysis/ml/advisor/.env.local` 填入自己的 `ML_ADVISOR_API_KEY`。模板已预设：

   ```dotenv
   ML_ADVISOR_ONLINE_ENABLED=1
   ML_ADVISOR_API_KEY=
   ML_ADVISOR_BASE_URL=https://aiping.cn/api/v1
   ML_ADVISOR_MODEL=DeepSeek-V4.1-Flash
   ML_ADVISOR_PROVIDER_LABEL=AIPing
   ```

   只需补齐空白 Key；模型名称和使用权限仍须与自己的 AIPing 账户一致。Key 保留在后端，
   `.env.local` 已被 Git 忽略，不写入 Vue、仓库或聊天问题。

3. 保留原有数据库配置，重启统一后端，并在网页重新载入参谋：

   ```bash
   python -m data_analysis.delivery.cli serve
   ```

4. 查看 `http://127.0.0.1:8000/api/v1/intelligence/advisor`。
   有效配置应返回 `onlineAvailable:true`、`defaultMode:"online"`、`onlineProvider:"AIPing"`。
   这一步只检查配置格式，不会发模型请求，不能证明 Key、余额、模型权限或网络已可用。
   在网页逐次勾选外发同意后发送“你好”，再询问“当前范围的充电服务瓶颈是什么？”并核对来源，
   才能检验自己的实际在线链路。

配置加载只读取参谋目录中的 `.env.local`，或进程环境变量 `ML_ADVISOR_ENV_FILE` 指定的文件。
进程中的同名 `ML_ADVISOR_*` 变量优先于文件：例如进程里仍为 `ML_ADVISOR_ONLINE_ENABLED=0`，
会覆盖文件里的 `1`；已有空 Key 环境变量也会覆盖文件值。遇到仍显示未配置时，先检查这些覆盖项。
不会读取其他应用的 `OPENAI_*`、`ANTHROPIC_*` 凭据，也不会执行文件中的 shell 表达式。
文件使用上面的五个 `名称=值` 配置项，允许空行和整行 `#` 注释；重复项、未知项或超过8KiB的文件不会加载。

`BASE_URL` 必须为 HTTPS API 根地址，程序去除末尾 `/` 后追加 `/chat/completions`。
不要填写完整的 `/chat/completions` 地址、Responses API 地址，或含账号密码、查询参数的 URL。
当前客户端使用 OpenAI-compatible Chat Completions JSON 协议；更换供应商须确认该协议及模型参数兼容。
Key不随仓库提供。当前配置环境已使用用户自己的Key实测 AIPing `DeepSeek-V4.1-Flash` 的规划、问候，
以及基于MySQL发布聚合的完整RAG回答，并核对返回的指标引用。其他部署仍须检查自己的Key、权限与网络；
这些成功用例不保证所有追问或模型解读都正确。

## RAG 怎样回答

1. 规划模型接收本次问题、有限历史和当前页面范围，选择 `chat`、`analysis`、`explanation` 或 `unsupported`。
   数据检索只能从 `overview`、`bottlenecks`、`stations`、`behavior`、`models` 五个主题中选最多三个，
   不能生成任意 SQL、URL、文件路径或写操作。
2. 后端按当前数据集、发布批次和筛选范围读取已有统计或模型报告，最多保留36项聚合证据。
   项目口径知识在 [knowledge.py](knowledge.py) 中维护，以中文双字切分及 BM25 本地检索最多4段。
   无需向量数据库或额外 embedding API；这里检索的是已维护的项目知识，不能当作任意文件上传问答。
3. 生成模型接收检索结果及明确的本次 `allowedCitationIds`，返回正文和引用。
   分析答复至少引用一项本次聚合证据；纯口径解释可只引用知识段落。
   若生成结果的引用不合法或缺少必要引用，最多再生成一次，要求使用允许的引用并删除无证据支撑的句子。
   纠错后仍执行相同的严格引用校验，未通过则返回错误。聊天答复使用 `status:"chat"`，不附加统计证据或伪造数据核验标记。

正常在线回答通常有“规划、生成”两次供应商调用；不支持或证据不足时可能提前结束。
只有上述引用纠错可追加一次生成，因此发生纠错时最多三次供应商调用。
所有阶段和纠错共享原60秒请求预算，单次模型调用最多45秒且受剩余预算约束。
网络、认证、限流等失败请求不自动重试；引用纠错不会重置超时预算。
当前使用非流式请求，等待完整回答后一次显示，不逐字输出。

引用校验能约束来源，不能证明每一句模型解读都正确。请对照引用中的数值、单位、范围和口径复核，
尤其不要把相关性说成因果，或把模型建议视为已验证的经营结论。不能承诺完全消除幻觉。

## 网页与对话历史

点击网页右下角人物展开聊天，再次点击人物、收起按钮或按 Escape 收起。
人物沿用 PR79 的 `whale-girl.png` 和 MIT 许可。Enter 发送，Shift+Enter 换行。
聊天内容和展开设置在窗口内滚动，输入区固定在底部；面板内滚轮不带动背景页面。

页面内保留最近10组已完成问答和草稿，切换页面或收起不会清除，刷新页面清除。
在线请求只带同一数据集、批次及筛选范围内最近三组问答，最多6条历史，每条最多800字符、合计最多4000字符。
历史用于理解追问，不是新的统计证据，也不能覆盖页面筛选。改变日期、城市或站点后须按新范围提问。
收起会停止接收未完成答复，并清除本次在线同意；已经发往供应商的请求无法撤回。

## 数据来源与外发边界

- 运营概况复用 `backend.service.overview`；瓶颈、站点比较和补能行为复用 `backend.advanced.analyze`；
  模型说明复用当前 `ForecastService`。证据与页面使用同一发布批次，不在 HTTP 请求中训练或另行加载模型。
- 每条聚合证据提供数值、单位、来源接口和字段；页面展示的来源保留实际批次及筛选参数。
  知识段落提供标题、正文和仓库内的口径来源。模型正文中的引用可与这些内容逐项核对。
- 统计日期采用 `Asia/Shanghai`，开始日期包含、结束日期不包含。正文或历史指定其他范围时应先调整控件，
  不能将整段统计当作单日或单站值。模型训练和评测仍使用固定历史留出范围，不随页面筛选改变。
- 全部业务数据为模拟数据。站点优先比较和站型×小时瓶颈至少需要30次尝试；这只是展示门槛，
  不保证统计显著性。异常分数不是故障概率，模型不提供实时设备安全诊断。
- 每次在线提问都须明确同意发送本次问题、有限历史、页面范围说明、聚合证据和检索知识。
  问题原文和历史文本经过下述处理后，会与检索内容一起发给已配置供应商。
  在线路径不检索或发送原始业务行、用户/会话明细及完整模型工件；对识别出的敏感编号、凭据和本地路径做拦截或移除，
  数据集/发布批次等内部标识不进入生成上下文。请勿在问题或历史中填写个人信息和凭据。
- Key 只用于后端请求认证，不进入模型提示、响应或来源。请求不能改变供应商、模型或地址，重定向被拒绝。
  参谋只读，不自动修改业务、执行预约/支付、安排维修或跳转页面。

## HTTP 契约与失败处理

GET 无需数据库或模型调用，返回配置可用状态、建议默认模式、供应商展示名称、外发说明和四个示例问题。
POST 使用项目标准 `{code,message,data,meta}` 包装。在线示例：

```json
{
  "question": "当前范围的充电服务瓶颈是什么？",
  "datasetId": "页面当前数据集",
  "publishedBatchId": "页面当前发布批次",
  "startDate": "2025-12-01",
  "endDate": "2026-05-30",
  "mode": "online",
  "consent": true,
  "history": []
}
```

可选 `cityId`、`stationId`，沿用城市归属及日期校验。省略 `mode` 的原始 POST 仍使用离线模式；
在线请求必须传 `mode:"online", consent:true`。`question` 最多300字符，请求体最多16KiB，未知字段拒绝。
`history` 只接受 `user`、`assistant` 两种角色及非空白字符串，并执行上述条数和字符上限。

`data` 包含 `status/mode/intent/answer/scope/evidence/knowledge/citations/limitations/suggestions`。
`knowledge` 项为 `{id,title,text,source}`；`citations` 是本次证据或知识的 ID 列表。
`status` 为 `answered`、`chat`、`unsupported` 或 `no_evidence`。
离线响应的 `knowledge`、`citations` 为空；导航建议仅指向已有 `overview/advanced/models/anomalies` 页面，由用户点击。

主要错误包括422输入无效或未同意外发、409批次冲突、503数据/模型未就绪或在线未配置、
429本地并发/频率限制或供应商限流、502供应商认证/模型配置/协议错误、504超时。
认证失败检查 Key 和模型权限，限流或额度不足检查账户，模型错误检查模型名和接口配置；
错误响应不回显供应商原文或密钥。

同一进程最多两个参谋工作线程，每个客户端每分钟20次。超时不提前释放仍运行的线程名额。
浏览器停止接收不等于撤销供应商请求，也不保证停止计费。

## 验证

```bash
python -m unittest data_analysis.tests.test_ml_advisor data_analysis.tests.test_advisor_rag data_analysis.tests.test_delivery_advisor -v
python -m data_analysis.contracts.delivery_schema --check
```

在线协议测试注入模型响应，不使用真实 Key，不产生外部模型调用。RAG/HTTP测试使用小型独立 SQLite 快照、
经过校验的聚合和模型替身，核对真实统计、知识来源、引用、最多一次引用纠错、范围、历史边界、逐次同意及超时后的并发控制。
完整 HTTP 测试需安装交付依赖，不能将缺依赖时的跳过当作通过。前端另运行 `npm test` 和 `npm run build`。
