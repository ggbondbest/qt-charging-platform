# AI 运营参谋(advisor):检索增强的只读 agent

**一句话**:大模型只负责"决定查什么、把查到的说成人话";所有数字必须来自本地工件
(冻结评测件、告警清单、观测记录、文档全文索引),逐条带来源,无依据就明说。

## 快速开始(mock 后端,零依赖零网络)

```bash
# 仓库根目录。第一步:生成参谋的"账本"(告警清单 + BM25 全文索引)
python -m data_analysis.ml.advisor.driver --prepare
# 已有产物默认复用;--prepare --force 强制重建。建到一半被打断留下的空库/空表
# 会被行数自检识别并自动重建(先 DROP 再 CREATE,不手工清也行)

# 提问(mock 是规则式脚本决策器,验证管线用,不代表模型能力)
python -m data_analysis.ml.advisor.driver "v5 模型在 TEST 段误报了几条?"
python -m data_analysis.ml.advisor.driver "TEST 段告警清单给我,挑一条下钻讲讲为什么"
python -m data_analysis.ml.advisor.driver "某站光伏板今天的发电量"   # → 应回答"无依据"
```

## 桌宠模式(前端入口)

```bash
# 终端 1:参谋小服务(标准库,不动队友 backend)
python -m data_analysis.ml.advisor.serve     # 127.0.0.1:8765,ML_ADVISOR_PORT 可换端口
# 终端 2:前端(队友的 vite,已在跑)→ http://localhost:5173 右下角鲸鱼娘桌宠
```

- 组件 `frontend/src/components/AiPet.vue`,App.vue 仅 2 行挂载(import + `<AiPet />`),
  不碰队友任何逻辑。零新 npm 依赖(立绘 PNG + CSS,没上 Live2D/Rive 这类重武器)。
  宠物连的端口可用浏览器 localStorage `ml_advisor_port` 覆盖(配合 `ML_ADVISOR_PORT`
  换端口起服务时用),默认 8765。
- **本地 HTTP 的四道闸门(CSRF)**:接口只有 `GET /health` 与 `POST /advisor/ask`。
  ① ask 的 GET 便车已删(405)——防 `<img src=/advisor/ask>` 这类"零预检执行";
  ② POST 锁 `Content-Type: application/json`(415)——form/text/plain、simple request
  都进不来;③ `Origin` 头存在则必须是 `localhost/127.0.0.1`(任意端口),否则 403
  ——CORS 只挡"读响应"不挡"执行",页面上任意网站借用户浏览器打本地服务这条路被掐死;
  ④ 请求体 `backend` 参数只认逐字 `"mock"` **只许降档离线演示、不许升档在线**——
  外部脚本即便绕过前三关,也逼不动 `.env` 里的真凭据去花用户的钱。
- **形象与状态机**:立绘与交互化用开源项目 [DeepSeek-Balance-Whale-Widget](https://github.com/MeteorNOX/DeepSeek-Balance-Whale-Widget)
  的 DeepSeek 鲸鱼娘(MIT © MeteorNOX,署名见 `frontend/src/assets/whale-girl.LICENSE.txt`);
  状态用 transform/filter 表达:思考时头顶冒描边小泡+轻呼吸、答对庆祝跳、连不上整只去色;
  可拖、悬停歪头、按压 Q 弹(参数取自原项目),拖到屏幕两侧 1/4 区自动**贴边吸附**、
  靠左吸附时镜像朝向;`prefers-reduced-motion` 下全停动画。面板色板吸收原项目
  深蓝描边 `#203170`/`#536ba9`,与立绘同调。
- **回答按 Markdown 渲染**(标题/列表/表格/代码块),组件内置迷你渲染器:先转义
  再拼白名单标签,模型输出里混进的 HTML 一律变文字,链接降级为纯文本(防钓鱼)。
- **出处说人话**:`friendly.py` 把内部路径/model_id 翻译成"异常检测·盲评考试记录"
  这类运营看得懂的标签;正文有 scrub 安全网(Windows 与 POSIX 绝对路径、裸表名、
  供应商名都在拦截面,工具报错文案也刻意人话化——错误文本会被模型逐字转述),
  模型不守规矩也由代码兜底。默认
  HTTP 响应里根本不含原始路径与后端供应商名(`ML_ADVISOR_SERVE_DEBUG=1` 才附)。
- 面板右上"自动/离线演示"下拉:选**离线演示**即强制 mock 后端——答辩现场断网也能演。
- **页面动作(跳转/预填)**:用户明确说"打开××页""把××填入××框"时,模型调
  `page_action` 工具,桌宠执行白名单动作——navigate 在回答打字机结束后自动跳转,
  fill 出确认芯片、点了才填,**只写值+发 input/change,永不代发 Enter/永不点提交
  按钮**(提交永远留给用户亲手点,执行回显带"未提交"提醒)。数量封顶:一次提问
  至多 1 次跳转 + 2 次预填,由代码层双闸硬截(agent 收集层 + friendly 出境层,
  不靠提示词自觉,超出静默丢弃)。安全架构:
  清单只有一份 `ai_actions.json`(navigate 7 页 / fill 8 框,随队友 #76 精简后的
  真实 DOM 收敛;admin 控制台与危险按钮刻意排除);模型只产意图,route/selector/label 执行参数永远从双端本地注册表
  resolve(后端 `actions.normalize` 校验 + 前端 `advisorActions.ts` 二滤 + 出境前
  `friendly._narrow_action` 终检,三道闸都不信模型措辞);新提问或关面板即作废未确认
  的填入。芯片状态**逐条动作独立**(不随消息级共享):一条失败可"↻ 再试"且不误标
  另一条 ✓;打字机后的自动跳转只在面板开着、且是最新一条回答时才执行(迟到回调
  不劫持旧消息)。执行器 fail-closed:跳转断言 `.app-shell.workspace-*`、分区断言
  aria-pressed、select 等 option 渲染、填后回读校验,任何一步不对都中止并如实回显。
  fill 写入前另过两门:**就绪门**——insights 查询框、预测起点框的值会被组件异步
  加载完成时无条件覆写,不等加载终态(grid 出现或报错 alert 出现,两种终态都放行)
  就写入,回读虽然一致、几百毫秒后值被页面悄悄改回,故未就绪直接拒写;
  **可见性**——`getClientRects()` 为空的元素(如大屏模式 v-show 收起的筛选区)
  照样能设值回读,但用户看不见,拒写不做"静默成功"。
  防漂移靠测试对账:manifest ↔ 前端注册表 ↔ 队友 App.vue 字面量三方互核,
  且钉死"动作字段逐字穿过 scrub 不变形"(machine 字段刻意不过 scrub)。
  **已知局限**:知识索引含用户评论/维修备注语料,理论上可被埋提示注入;但动作出境
  形状被"三道闸+封顶"锁死在注册表键与合法值内,注入改变不了执行面,至多让措辞
  怪异——且出处逐条展示在面板里,用户可查账。
- 服务没起:宠物变灰标"离线",错误气泡给启动命令+一键重试;回答中可点 ■ 停止;
  长回答贴底才自动滚,滚走了出"回到底部"按钮。
- Windows 小坑:别用 curl 命令行直传中文(argv 会被转成 GBK 字节,服务端回 400),
  浏览器/Python 客户端都是 UTF-8,没问题。

## 接真实模型(DeepSeek 双兼容口)

`.env`(复制 `.env.example`)里写 key 即切换后端;两个协议都支持,一个文件的事:

| 路线 | 配置 |
| --- | --- |
| DeepSeek OpenAI 口 | `ML_ADVISOR_BACKEND=openai` + `OPENAI_BASE_URL=https://api.deepseek.com` + `OPENAI_API_KEY=...` + `ML_ADVISOR_MODEL=deepseek-v4-pro` |
| DeepSeek Anthropic 口 | `ML_ADVISOR_BACKEND=anthropic` + `ANTHROPIC_BASE_URL=https://api.deepseek.com/anthropic` + `ANTHROPIC_AUTH_TOKEN=...`(需 `pip install anthropic`) |

后端选择**显式优先**:环境里残留的其他 `*_API_KEY` 不会触发联网——只有本目录
`.env` 写了 key 或显式指定 `--backend`,才会离开 mock。凭据防呆见 `config.py`。

## 架构

```
问题 → agent.py 循环(max 6 轮)
        ├─ 后端 llm.py:mock | OpenAI 兼容 | Anthropic 兼容(中立 transcript 双向渲染)
        ├─ 工具 tools.py(全部只读,输出 JSON 必带 source):
        │    list_models / read_test_metrics / query_alerts / explain_session
        │    lookup_weather_calendar / lookup_maintenance / queue_summary
        │    station_profile / review_feedback / search_knowledge(FTS5/BM25)
        │    page_action(白名单页面引导:不写数据、不代提交,清单在 ai_actions.json)
        └─ 出处与动作凭证收集在代码层做,不靠模型自觉
产物 artifacts.py → outputs/ml_advisor/{alerts_v5.csv, knowledge.db}(gitignored)
```

## 为什么"检索"以结构化查询为主、向量库不建

数据九成是结构化工件(评测 json/告警表/时序/工单),精确条件问题上 pandas 过滤
就是最强检索;适合文本检索的只有模型卡、评论、维修备注一小部分,sqlite FTS5
(标准库自带,BM25,CJK 逐字切分)足够,语料规模用不上向量库——且 Anthropic
无 embeddings API,引入等于多绑一家供应商。对外表述仍成立:**检索增强生成,
答案只来自平台工件并附出处**(RAG 的本质是纪律,不是某个向量库)。

## 红线(与全仓一致)

- 只读:`anomaly_labels` 真值永不进工具;告警清单=模型输出侧;分型命中只能读冻结件汇总。
- 不碰盲评工件,不训练,不写 datasets/;派生物只落 outputs/ml_advisor/。
- 表述上限:**模拟数据测试结果**(系统提示词第 3 条硬编码)。
- 断网/无 key/mock 三种情况行为一致:该"无依据"就"无依据"。

## 测试

```bash
python -m unittest data_analysis.tests.test_ml_advisor -v
# 真 key 冒烟(会计费,默认跳过):
ML_ADVISOR_LIVE=1 python -m unittest data_analysis.tests.test_ml_advisor -v
```

## 实测记录(2026-09-15,deepseek-v4-pro via api.deepseek.com)

```
$ driver "TEST 段告警清单给我,挑一条下钻讲讲为什么"
[r1] query_alerts(TEST) → 91 条
[r2] explain_session(SES-00113956)     ← 模型自己挑了分数最高的一条,不是脚本
答:…tempW z=10.0 极端离群为主因…stop_reason=TARGET_REACHED 说明是过程温度问题…
[来源:bundle/alerts/charging_sessions] 且自带"模拟数据测试结果"表述
```

DeepSeek 实测两坑已垫平:`deepseek-v4-pro` 默认思考,小 `max_tokens` 会被
reasoning 吃光(finish=length 空正文)→ 协议层默认发 `thinking:{type:"disabled"}`
(官方文档对工具调用场景的推荐)并带降级重试;`ML_ADVISOR_THINKING=enabled` 可开回。
Anthropic 口的 `reasoning_content` 回显要求属于老 V3.x 思考模式,v4-pro 实测
tool_calls 响应无该字段,暂不需要;若换回老模型报错,先查 llm.py 这里。
