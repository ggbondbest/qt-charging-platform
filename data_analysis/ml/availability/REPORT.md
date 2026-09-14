# 空闲桩数可用性预测 · 交付说明（机器学习成员 B）

读者：组长（用于接后端注册、评审与冻结验收）。
代码基线：本轮已 rebase 到 `origin/develop @ d8b5fa2`（含 PR #63 清洗验收、#64 180 天交付重发布、
#65 分析服务化迁 MySQL）；此前版本基于 `444b3fd`（已合入 `develop 067f7b9`）。
本地分支：`feature/phase2-ml-load`，**本轮已按组长要求推送到 `origin` 并建立上游追踪**；分支名写的是
load，本分支实际内容是 availability（B 线），建议改名，见第 7 节。
**rebase 带来的后果不是零**：已发布产物与服务批次的绑定现在会被推理入口拒绝，见第 2.1 节与第 5.11 节末。

> 全部指标为模拟数据测试结果（第二阶段发布批次），不代表真实运营数据表现。

## 0. 一句话结论

交付 1 / 6 / 24 小时三个跨度的**空闲充电桩数量**概率预测：每个未来小时一个有序分类
HistGradientBoosting 全分布 + 站点×小时层级经验先验，服务口径 TEST MAE
**0.4816 / 0.5047 / 0.5120 桩**，输出合法率 **1.0000**，`predict()` 直接通过
`contracts/model.py:validate_prediction`，**不改任何契约**。

> **本轮状态提示（rebase 之后，已改正）**：上游把数据批次重发布了，我原来的已发布 bundle 会被推理入口
> 按批次拒收。已按第一条出路处理——**B 线产物按 `analytics-298aa3ee…` 重跑并重新发布**
> （`ml_avail_run5` / `ml_avail_run6` / `ml_avail_eval_run6_v1` / `ml_avail_forecasts_v2`），
> **分数一分没变**：24 份 bundle 的指标块逐位相同、评估报告只有头部 6 行被替换、预测 CSV 两份 sha256 相同；
> 变的是批次绑定，全过程与证据在第 5.12 节，套件已回到 `Ran 67 tests … OK`（同节末段）。

## 1. 对照交接文档的交付清单

| 文档对 B 的要求 | 交付位置 | 状态 |
| --- | --- | --- |
| 建立 `ml/availability/` | `data_analysis/ml/availability/`（9 个模块 3,260 行 + `__init__.py` 33 行；共享层 `ml/common/` 6 个模块 900 行；测试 `ml/tests/test_ml_contract.py` 1,202 行 / 67 个用例。行数口径＝每文件 `Get-Content` 的元素个数、含空行，改过代码就要重数——本轮这三个数就是重测过的） | 完成 |
| 预测各未来小时**最后采样时刻**的空闲桩数 | `model.py` 标签 `label_available_count_hNN` = 应答小时最后采样时刻的空闲桩数；口径由 `train.py` 与 `test_the_label_is_the_free_count_of_the_hour_it_answers` 双向锁定 | 完成 |
| 先建基线，再训练树模型 | `evaluate.py` 六级基线阶梯（重复上小时 / 站点×小时中位数 / 站点×小时经验分布 / 场站类型×小时 / 城市×小时 / 全局中位数），只在 TRAIN 拟合 | 完成 |
| 树模型 | `train.py`（0.2.0 纯分类器）+ `build_hierarchical.py`（0.3.0 出厂先验版，复用 0.2.0 的估计器，只在 VALIDATION 上选 k、区间级别和点值规则） | 完成 |
| 评估 | `evaluate.py` → `evaluation_report.md/.json`（分跨度、分城市/站点、冷启动留出、区间覆盖、风险阈值） | 完成 |
| 评价报告 | **`outputs/ml_avail_eval_run6_v1/evaluation_report.md`**（本版为准：与发布批次 `analytics-298aa3ee…` 绑定；除头部 6 行——批次、被评分目录、导出清单 sha、生成时间与耗时、复现命令、"写入新目录"那句——之外与 `ml_avail_eval_run2_v2` 逐字符相同，旧目录一字未动，见第 5.12 节）。因 `outputs/` 被 gitignore，关键数字全部抄进本文件第 5 节 | 完成 |
| 推理 | `predict.py::AvailabilityForecaster.predict()`（契约点预测）与 `.risk()`（小数预计值、区间、P(无桩)，独立侧信道） | 完成 |
| 报告 MAE 与输出范围合法性 | 第 5 节表格；合法率 1.0000，且是结构性结果（模型只能输出 0..capacity 整数，推理再裁剪一次） | 完成 |
| 不能把预计小数桩数当真实库存 | 对外 `points[].value` 恒为 Python `int`；期望值只在 `risk()` 的 `expectedChargers` 里，字段名与 `definition` 文案都标注为预计值；由 `MinimalInferenceTest` 机械检查 | 完成 |
| 模型文件及预处理器 | `data_analysis/outputs/ml_avail_run6/{h01,h06,h24}/model.joblib`（bundle 内含特征列顺序、站点/日历/城市档案、先验表 → 预处理与模型同包，不需要第二个文件；批次号打在 pickle 里，所以换批次必换 `artifactSha256`，见第 5.12 节第 5 点） | 完成 |
| `model_metadata.schema.json` 要求的元数据 | 每 bundle 的 `model_metadata.json`，由 `artifacts.build_metadata()` 生成并 `validate_metadata()` 校验；`metrics` 严格只含 `mae/rmse/testSamples/unit` | 完成 |
| 可复现训练命令 | 本文件第 3 节；评估报告头部自带复现命令行；**当前这批产物（`ml_avail_run5` / `ml_avail_run6`）的 24 份 `training_report.json` 每份都带 `reproducibleCommand` 与 `seed`**——打开产物即可看命令，不必照第 3 节重敲（旧的 run1/run2 先于该字段，见 3.1；代码侧 `artifacts.invocation()`） | 完成 |
| 最小推理测试 | `python -m data_analysis.ml.availability.predict --self-check --run-dir ...`，并同步为 `ml/tests` 的 `MinimalInferenceTest`（4 个用例）；另有 `RunDirectoryGuardTest`（5 个，锁"不覆盖已发布 run"）与 `InvocationRecordTest`（3 个，锁复现命令本身可执行） | 完成 |
| 应组长追问「能不能用更好的模型」 | `model_search.py` → `outputs/ml_avail_exp_h01_v1`、`ml_avail_exp_h06_v1`、`ml_avail_exp_h24_v1`、`ml_avail_exp_h06_test1`（第 5.7 节。结论：赢的是**特征**不是估计器，且两种做法不叠加；因此**未**发布 0.4.0，理由写在同节） | 完成（未发版，待组长定） |
| 应组长要求「先训练 5 个 epoch」 | 树模型没有 epoch，如实换算成 `--rounds-multiplier`（第 5.8 节）；5× 预算全量重训已跑完 → `outputs/ml_avail_run3_r5`（模型改名 `-r5`，不冒充已发布身份） | 完成（结论：不值得发版） |
| 应组长要求「可以断点续训」 | `train.py --resume` 与 `build_hierarchical.py --resume`（第 5.9 节：829.7 s 的全量训练断在第 6 个 bundle，续跑 380.7 s 补完，12 个 bundle 与一口气跑完逐位相同；配方不符即拒绝且不动 checkpoint） | 完成 |
| 应用侧可直接看的预测表 | `outputs/ml_avail_forecasts_v2/forecast_table.md` + `forecasts.csv`（4,650 行，`forecast_table.py` 产出，第 5.10 节；与上一批的 `ml_avail_forecasts_v1/forecasts.csv` **sha256 相同**） | 完成 |
| 独立扩展（择一） | **未做**。按「时间不足时先交付基础预测闭环」，先把基础线补完；扩展建议与数据前提见第 6 节 | 待组长定 |

## 2. 输入批次与绑定（全部实测，非文档转抄）

| 项 | 值 |
| --- | --- |
| datasetId | `charging_full_180d_v2` |
| pipelineRunId | `spark-6ed381b125034f9e95d726a74befb121` |
| publishedBatchId | `analytics-298aa3ee1401461fb06ea2bb96930dcf`（上一批 `analytics-5f8e9342…` 见第 2.1 节） |
| 原始数据清单 sha256（`sourceManifestSha256`） | `1f03e37c644ec928f8de0511b614b316bcc35632056891173d2377b2733ccb95`（**两批相同**） |
| 导出清单 sha256（仅记录，不写进 `sourceManifestSha256`） | `7e0e50665fa803e9e2f9150c77e93538d942ead96504aa0a9f572d4e6a783350` |
| 权威 run 目录 | `ml_avail_run5`（基座 0.2.0）/ `ml_avail_run6`（出厂 0.3.0）；名字只写在 `ml/availability/__init__.py` 的 `BASE_RUN` / `HIERARCHY_RUN` 一处 |
| featureVersion | `history24-v1`，模型输入 75 列（不含任何 `label_` / `split_` 列，由测试锁定） |
| 训练帧 | 107,425 行 × 75 特征；25 站、5 城（BJ/DL/SH/SY/SZ）、5 种场站类型；每站 `capacity = 3` 桩，故标签取值 {0,1,2,3} |
| 切分（`mlSplits`，上海业务日右开） | TRAIN 2025-12-02→2026-03-30，VALIDATION →2026-04-29，TEST →2026-05-29 |
| 各跨度行数 | 1h 70,800/18,000/18,000/EXCLUDED 625；6h 70,675/17,875/17,875/1,000；24h 70,225/17,425/17,425/2,350 |

**批次绑定纪律**：`sourceManifestSha256` 绑的是**原始包 manifest**（`1f03e37c…`）。导出包
`serving_manifest.json` 自身的哈希（上一批 `358eeba3…`，本批 `7e0e5066…`）另记在
`training_report.json` 的 `servingManifestSha256`。测试
`test_bundle_is_bound_to_the_batch_it_reads` 明确断言二者不相等——负荷线 0.1.0 产物正是把后者
误写进了前者（见附录 A）。

### 2.1 rebase 到 develop 之后：上游把批次重发布了（本轮实测，已按第一条出路改正，见第 5.12 节）

本分支 rebase 到 `origin/develop`（`d8b5fa2`，含 PR #64 `data(analysis): refresh 180-day cleaned
delivery`、PR #65 MySQL 服务化）后，`data_analysis/datasets/analytics_full_180d_v1/` 里的导出被换成
新的一次发布：

| 字段 | 我的产物记录的值 | 现在仓库里的值 |
| --- | --- | --- |
| `publishedBatchId` | `analytics-5f8e93429948404099b735999bfb6c4e` | `analytics-298aa3ee1401461fb06ea2bb96930dcf` |
| `pipelineRunId` | `spark-625439908b6d4bcca1ae72e2e35ef27f` | `spark-6ed381b125034f9e95d726a74befb121` |
| `sourceManifestSha256` | `1f03e37c…` | `1f03e37c…`（**未变**：原始包没换，变的只是这次 Spark 重跑导出的批次号与各分片哈希） |
| 训练帧 | 107,425 行 × 75 特征，TRAIN/VAL/TEST 70,800/18,000/18,000 | 同左，**一字未变** |

后果按事实记（这一段描述的是**重绑之前**的状态，重绑记录在第 5.12 节）：**这套用例当时在本分支上是红的**
（逐条计数见第 5.11 节）。45 个红项拆开是
24 项 `test_bundle_is_bound_to_the_batch_it_reads` 子项的直接断言
（`'analytics-5f8e…' != 'analytics-298a…'`）、19 项在构造特征、给出任何预测之前就抛出的
`PredictionError: BATCH_MISMATCH`（含那条"请求不能偷渡答案"的防泄漏用例——它现在到不了断言）、1 项
`--self-check` 退出码非 0、外加 1 项连带失败的聚合断言（"所有已发布 bundle 要么 PASS 要么显式
SKIPPED"——因为没有一个 PASS）。
**没有任何一条模型质量断言变红**。而 `ResumeEndToEndTest` 那两条真命令行用例恰好在重训：同 seed
（20260913，`train.py` 的默认值）+ 同配方在新批次上现训 h01，日志打印
`TEST MAE 0.527 / RMSE 0.726 / n=18000`、续跑复用那份打印 `0.5274`——与当时已发布（现已被第 5.12 节
的 `ml_avail_run5` 取代）的 run1 h01 记录的 `mae 0.5274 / rmse 0.7259 / n 18000` 一致（该用例自己断言的是
"续跑 == 现训"，与 run1 的相等是我照着两边数字对读出来的，不是它断言的）。所以这是**批次号变更导致的
重新绑定问题**，不是数据内容变了、也不是分数塌了。当时摆着两条出路：把 B 线产物按新批次重跑一遍再发，
或者数据层确认"每次重发布都换 batchId、下游产物随之失效"这条口径本身是否是验收想要的答案。
**组长选了第一条（「那你就根据问题来改正」），本轮已执行完，全部实测记录在第 5.12 节**；第二条作为
口径问题仍然留着（第 7 节第 5 条）。

## 3. 启动方法（仓库根目录执行，需 numpy/pandas/scikit-learn/joblib）

```bash
# 0) 数据画像（可选，只读导出包并逐分片校验 manifest）
python -m data_analysis.ml.availability.prepare_data --output data_analysis/outputs/ml_avail_run5

# 1) 树模型：纯有序分类器 + 三个城市的冷启动留出（seed 20260913）
python -m data_analysis.ml.availability.train \
    --output data_analysis/outputs/ml_avail_run5 \
    --holdout-city DL --holdout-city SY --holdout-city SZ

# 2) 出厂先验版：复用第 1 步的估计器，只在 VALIDATION 上选每层 k / 区间级别 / 点值规则
python -m data_analysis.ml.availability.build_hierarchical \
    --source-run data_analysis/outputs/ml_avail_run5 \
    --output data_analysis/outputs/ml_avail_run6

# 3) 评估（写新目录，绝不覆盖已发布报告；报告头会自动记下这条命令）
python -m data_analysis.ml.availability.evaluate \
    --run-dir data_analysis/outputs/ml_avail_run6 \
    --output data_analysis/outputs/ml_avail_eval_run6_v1

# 4) 最小推理测试：12 个 bundle 逐个加载、按契约服务一个确定 TEST 行，任一非法即退出码 1
python -m data_analysis.ml.availability.predict --self-check --run-dir data_analysis/outputs/ml_avail_run6

# 5) 单次推理（后端适配器的调用形态）
python -m data_analysis.ml.availability.predict \
    --bundle data_analysis/outputs/ml_avail_run6/h06 \
    --station ST-BJ-01 --reference-time 2026-04-28T19:00:00Z --horizon 6 \
    --export data_analysis/datasets/analytics_full_180d_v1

# 6) 预测表（人看的那份；`--run-dir` 默认即上面的出厂 run）
python -m data_analysis.ml.availability.forecast_table \
    --run-dir data_analysis/outputs/ml_avail_run6 --reference-hours 6 --display-hours 10 \
    --output data_analysis/outputs/ml_avail_forecasts_v2
```

`run1 ~ run4` 是**上一批次（`analytics-5f8e9342…`）上的历史产物与预算实验**，命令形态与上面完全一致，
只是目录名不同（`ml_avail_run1` / `ml_avail_run2`，以及 `--rounds-multiplier 5` 的
`ml_avail_run3_r5` / `ml_avail_run4_r5_resume`）；它们在服务路径上会被
`PredictionError: BATCH_MISMATCH` 拒，保留只为对照与旧格式回归（第 2.1、5.12 节）。
中途断了把第 1 或第 2 步原样重敲、末尾加 `--resume` 即可接着跑（第 5.9 节）。

三个**会写产物**的入口（`train` / `build_hierarchical` / `evaluate`）都在装载数据帧之前拒绝覆盖：
目标目录已有 bundle、已有 `train_summary.json` 或已有 `evaluation_report.*` 时直接退出，要求写新目录；
`predict` 不写文件，仍由 `--self-check` 的退出码把关。这三条拒绝各有单元测试
（`RunDirectoryGuardTest` 与 `AuditScriptTest`），所以"不覆盖已发布产物"是可核查的约定，不是口头承诺。
`train` / `build_hierarchical` 的 `--resume` 只放开"目录里已有 bundle"这一半：只要汇总文件还在，
仍然拒绝——续跑不能把一份已经被引用过的数字变成混合产物（第 5.9 节）。

### 3.1 可复现性的现状与边界（如实说明）

- 随机种子：`--seed 20260913`（train/build_hierarchical 默认值）。run 级文件
  （`train_summary.json`、`hierarchical_report.json`）记录了 seed 与两个 manifest 哈希；
  **依赖版本**记在每个 bundle 的 `model_metadata.json` 的 `dependencies` 字段里
  （`artifacts.dependency_versions()`：python/numpy/pandas/scikit-learn/joblib/platform），
  因为那才是加载产物真正需要的东西。
- 本次新增 `artifacts.invocation()`：此后每一次训练产出的**每个 bundle** 的 `training_report.json`
  都会带 `reproducibleCommand`（含全部命令行参数）与 `seed`，`evaluate.py` 的报告头同样自带复现命令行
  （`ml_avail_eval_run2_v2` 起就是这样产出的，重绑后的 `ml_avail_eval_run6_v1` 同）。`-m` 运行的模块名
  从 run spec 还原，不会写成 `__main__`
  ——这条由 `InvocationRecordTest` 锁住，因为一份写着 `-m __main__` 的报告等于没有复现命令。
- **旧的 run1 / run2 不含该字段**（它们先于这次改动，且已被第 5.12 节的重绑取代）。本轮递归读过
  `ml_avail_run1`、`ml_avail_run2`
  全部 **24 份** `training_report.json`（每个 run 是 3 个主跨度 + 3 座留出城市 × 3 = 12 个 bundle）：
  `reproducibleCommand` **24/24 为空**；`seed` 只有 run1 的 12 份写了（`20260913`），
  run2 的 12 份**这个键根本不存在**（本轮按 `"seed" in doc` 逐份数的：absent 12/12，不是写了 `null`——
  用 `doc.get("seed")` 读会把两种情况都读成 `None`，上一版就是据此写成"为 null"的），
  因为当时 `build_hierarchical` 只在 run 级 `hierarchical_report.json` 记 seed。
  补齐的代价分两层，别混为一谈（这是上一版本报告写错的地方）：
  * **服务产物（run2 的 12 个 bundle）重跑只要 90 秒**——它们由 `build_hierarchical` 产出，
    当时日志记录的实测耗时是 **88.3 s**。但重跑写进 bundle 的是**构建命令**，
    仍然不含训练命令，因为 `--source-run` 指向的 run1 没记下自己的命令。
  * **要让产物真的带着训练命令，必须重跑 `train`**——本轮已经实测了这条命令的代价：全 12 个 bundle
    在 **5× 预算**下 **829.7 s**（`ml_avail_run3_r5/train_summary.json`，1× 只会更快），
    而不是上一版这里写的"约 69 分钟"。那个 69 分钟是 run1 目录的时间戳跨度
    （21:34:32→22:43:27，当时那一坐里夹着别的工作），**不等于纯训练时间**，本轮没能复现它，
    它也不该被拿去当重跑预算。
  * 携带性已被证实：`ml_avail_run3_r5` 的 12 份 `training_report.json` **每一份**都带
    `reproducibleCommand` 与 `seed`（当场可核，`grep` 即可），run1/run2 则一份都没有。
  上面第 3 节的命令按各 run 报告里记录的参数与模块默认值重建，等价但不逐字节证明当时输入。
  **这条"要不要为这个字段重跑"的问题已被第 5.12 节的重绑消解**：当前这批产物（`ml_avail_run5` /
  `ml_avail_run6`）的 **24/24 份 `training_report.json` 都带 `reproducibleCommand` 与 `seed`**（本轮当场数过），
  所以可复现路径现在是"打开产物看命令"，不再需要先照第 3 节重敲。旧 run1/run2 仍是缺这个字段的状态，
  而且它们绑的是上一批次，本来就不能再服务。
- 训练是确定性的（同一 seed、同一批次、同一 sklearn 版本 ⇒ 同一 MAE）。**跨版本不可加载**：
  本批产物为 Python 3.13.9 / numpy 2.4.4 / pandas 2.3.3 / scikit-learn 1.7.2 序列化，
  Linux 成员环境须按 `model_metadata.json` 的 `dependencies` 对齐，否则 pickle 加载即崩。

## 4. 推理接口：是否影响契约

**不影响任何共享契约。**

- `predict()` 返回的仍是 `contracts/model.py` 规定的点预测：每点仅 `{"timestamp","value"}`，
  连续整点，`unit = "chargers"`，`value` 为 `0..station capacity` 的整数；`schemaVersion` /
  `featureVersion` / `modelId` / `modelVersion` 全部取自 bundle 的元数据，经
  `validate_prediction(result, context, capacity, task="availability")` 出口校验。
- 区间、期望值、P(无桩) **不塞进契约**，走 `risk()` 独立侧信道（`predict.py` 内已写明理由：
  给已发布指标换含义或加点字段属于契约变更，须组长动手）。
- 服务前置校验：`reference_time` 必须是整点、必须提供严格早于它的 **24 个完整连续**站点小时；
  缺行、含空、混入未来行 → `HistoryError`；`datasetId`/`publishedBatchId` 与 bundle 不一致 →
  `PredictionError("BATCH_MISMATCH")`；未收录站点 → `STATION_NOT_FOUND`。这些正是统一错误处理需要的
  错误码，组长接入 `backend` 时可直接映射。
- 冷启动 bundle（`coldstart_*`）`excludeCity` 非空，**不得**对外服务该城市；`--self-check` 与测试
  都显式跳过并说明原因。

## 5. 测试结果（模拟数据，单位 = 空闲充电桩个数，不是百分比；MAE 0.5 = 平均偏差半个桩）

### 5.1 出厂模型 `ml_avail_run6`（0.3.0，服务口径，TEST）

（目录自第 5.12 节的重绑起换成 `ml_avail_run6`；下表与重绑前 `ml_avail_run2` 的那一份**逐字符相同**——
新版评估报告 125 行里只有头部 6 行被替换，其余全是同一批分数行。）

| 跨度 | MAE | RMSE | 覆盖率@80% | 平均区间宽度 | 输出合法率 | P(无桩) AUC | 评分点数 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| h01 | 0.4816 | 0.7223 | 0.8111 | 0.715 | 1.0000 | 0.8767 | 18,000 |
| h06 | 0.5047 | 0.7482 | 0.8158 | 0.840 | 1.0000 | 0.8725 | 107,250 |
| h24 | 0.5120 | 0.7562 | 0.8173 | 0.872 | 1.0000 | 0.8700 | 418,200 |

12/12 个 bundle 的服务口径 MAE 与其出厂 `model_metadata.json` 逐位一致——评估走的就是推理路径，
没有第二套实现。

### 5.2 对基线（同一批 TEST 行，基线只在 TRAIN 拟合、按应答小时分桶）

| 跨度 | 模型 MAE | 最强基线（站点×小时历史中位数） | 下降 | 重复上小时 | 场站类型×小时 | 城市×小时 | 全局中位数 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| h01 | 0.4816 | 0.5281 | −8.8% | 0.6068 | 0.5597 | 0.8919 | 0.9690 |
| h06 | 0.5047 | 0.5298 | −4.7% | 0.8981 | 0.5617 | 0.8910 | 0.9683 |
| h24 | 0.5120 | 0.5299 | −3.4% | 1.0955 | 0.5611 | 0.8910 | 0.9679 |

版本内部收益（0.2.0 纯分类器 → 0.3.0 出厂先验）：**−8.7% / −10.8% / −11.4%**
（0.5274 / 0.5658 / 0.5780 → 0.4816 / 0.5047 / 0.5120）。这 **不是三份可相加的功劳**：

1. 点值规则从众数改为分布**中位数**（MAE 最优），贡献 −8%~−9%，是大头；
2. 站点×小时层级先验混在**分布**上，主城市再降 0.6%~1.9%；
3. 最强基线本身（0.528~0.530）只比全局中位数 0.97 好、比模型差——但它是必须摆出来的对手。

`ml_avail_run1/evaluation_report.md`（0.2.0 时期）**不可引用**：该版审计脚本把收缩权重
`k/(n+k)` 记在查表一侧，方向用反，其「模型 MAE」连自身元数据都对不上。修正记录写在
`ml_avail_eval_run2_v2/evaluation_report.md` 第 8 节（重绑后的 `ml_avail_eval_run6_v1` 第 8 节同文）；
`ml_avail_eval_run1_corrected` 是用修正后的
审计脚本重跑 run1 的结果。

### 5.3 用户口径的"准确率"（整数点命中率，h01 / h06 / h24 主模型）

| 跨度 | 完全命中 | ±1 桩以内 |
| --- | --- | --- |
| h01 | 58.0% | 94.0% |
| h06 | 55.9% | 93.9% |
| h24 | 55.4% | 93.6% |

对照 0.2.0（众数点值）：59.3% / 56.8% / 56.1% 与 89.3% / 88.3% / 87.9%。
即**中位数让 MAE 降 8~9%、±1 以内升约 5 个百分点，但完全命中反而降约 1.3 个百分点**——
中数是 MAE 最优，不是众数。两个数必须一起报，不能只挑好看的。

### 5.4 无桩风险提示（h01 主模型，自然基线率 0.2050）

| 阈值 | 触发时点数 | 精确率 | 召回率 |
| --- | --- | --- | --- |
| P(无桩) ≥ 0.30 | 5,561 | 0.504 | 0.759 |
| P(无桩) ≥ 0.50 | 2,339 | 0.673 | 0.427 |
| P(无桩) ≥ 0.70 | 916 | 0.842 | 0.209 |

模型 AUC 0.8767；作为对照，「站点×小时经验分布」查表的 P(无桩) AUC 为 0.8619，也是强对手。

### 5.5 冷启动（整城剔除后训练，只用该城 TEST；基线同样只用其余城市 TRAIN）

| 留出城市 | MAE（h01 / h06 / h24） | 相对主模型同城退化 |
| --- | --- | --- |
| DL | 0.4900 / 0.5226 / 0.5249 | +3.6% / +5.2% / +3.9% |
| SY | 0.4756 / 0.5083 / 0.5195 | +3.6% / +3.9% / +3.9% |
| SZ | 0.5122 / 0.5431 / 0.5501 | +1.7% / +3.0% / +3.0% |

**反面证据也一并交付**：借来的场站类型/城市层在陌生城市的 TEST 上净效果介于 −0.1% 与 +1.3%
（负号才是变好），9 行里有 3 行权重为 0（VALIDATION 判 `off`）。量级与验证噪声同阶，
**不足以宣称冷启动也能从查表稳定获益**；能说的只有"没有本地历史的站，纯模型 MAE ≈ 0.49–0.55，
退化在 5% 以内"。

### 5.6 分城市与最差站点（主模型，服务口径，TEST；该表自 `ml_avail_eval_run2_v2` 第 6 节起存在，重绑后的 `ml_avail_eval_run6_v1` 第 6 节同表同数）

| 城市 | h01 MAE | h06 MAE | h24 MAE | h01 点数 | h01 相对全体 |
| --- | --- | --- | --- | --- | --- |
| BJ | 0.4864 | 0.5103 | 0.5140 | 3,600 | +1.0% |
| DL | 0.4728 | 0.4970 | 0.5052 | 3,600 | −1.8% |
| SH | 0.4861 | 0.4996 | 0.5068 | 3,600 | +0.9% |
| SY | 0.4589 | 0.4892 | 0.4998 | 3,600 | −4.7% |
| SZ | 0.5036 | 0.5274 | 0.5340 | 3,600 | +4.6% |

城市间差异很小（h01 极差 0.045 桩），且每城 3,600 点样本量相同；DL/SY/SZ 三城本表数值与第 5.5 节
"主模型同城 MAE" 逐位相同，是同一批行的两条独立渲染路径，互为校验。
误差最大的站点（h01，每站 720 点）：`ST-DL-04` 0.7736、`ST-SZ-04` 0.7667、`ST-SH-04` 0.7611、
`ST-BJ-04` 0.7569、`ST-SY-04` 0.7153，其余 20 站 ≤0.5153——**最差的前五名恰好是每城的 04 号站**，
说明误差不是均匀分布的。本轮把"为什么"量到底了（`probe_cell_floor.py` / `probe_difficulty.py`，
临时脚本未入库，数字抄在这里）：

| 04 号站 | 类型 | 只用该站历史的**最好常数**预测 MAE | **(站×小时) 查表中位数** MAE | 出厂模型 MAE | 模型−查表 |
| --- | --- | --- | --- | --- | --- |
| ST-SZ-04 | transit | 0.8931 | 0.7931 | 0.7667 | −0.0264 |
| ST-SY-04 | transit | 0.8014 | 0.7431 | 0.7153 | −0.0278 |
| ST-SH-04 | transit | 0.8403 | 0.7403 | 0.7611 | +0.0208 |
| ST-DL-04 | transit | 0.8458 | 0.7403 | 0.7736 | +0.0333 |
| ST-BJ-04 | transit | 0.8347 | 0.7264 | 0.7569 | +0.0305 |
| 其余 20 站 | 混合 | 0.7361…1.1153 | 0.3833…0.5458 | ≤0.5153 | — |

三条结论，先前那句"推测摆动更多"已被实测替换：

1. **不是"分布更平"**：按该站 TRAIN 历史算边际熵，transit 五站逐站 1.831–1.971 bit，
   transit 均值 1.8934 与 residential 均值 1.8892 无差别；常数下限最高的其实是 shopping
   （`ST-SY-02` 1.1153、`ST-DL-02` 1.0403）。
2. **是"小时规律本身不稳"**：查表能把 shopping 五站的 0.9917 压到 0.4850（−0.51 桩），对 transit
   五站却只能把 0.8431 压到 0.7486（−0.09 桩）——(站×小时) 这一族规则能拿到的收益，
   恰好就在它们身上最小。
3. **模型没有在这五站失职**：出厂模型与"该站该小时的查表中位数"互有胜负 ±0.03 桩，而每站 720 点
   的 MAE 标准误约 0.026 桩，差异在噪声内。也就是说这 0.25 桩的额外误差不是重训一个更强的
   (站×小时) 模型能消掉的，需要新的信息源（例如小时内实时占用/在途会话）。

接入侧若要按站点做告警阈值，应先单独看这几站的曲线，不要把城市级 MAE 当站点级用。

### 5.7 「能不能用更好的模型」——检索结果与不发版理由（应组长追问，本轮新增）

问的是模型，答案落在特征上。检索脚本 `model_search.py`（本轮入库）在**同一批行、同一划分、同一 seed**
上比较配置，只在 VALIDATION 上选型，TEST 最后一次性确认（`--confirm-test` 才会碰 TEST，且单独写目录）：

| 跨度 | `control`（现行配方，无先验特征） | `cell_features`（把 (站×小时) 历史分布当 8 列特征喂进去） | 变化 |
| --- | --- | --- | --- |
| h01 VALIDATION | 0.4984 | 0.4867 | −2.3% |
| h06 VALIDATION | 0.5307 | 0.5145 | −3.1% |
| h24 VALIDATION | 0.5416 | 0.5241 | −3.2% |
| h06 **TEST**（一次性确认） | 0.5147 | 0.4992 | −3.0% |

四点如实说明：

1. h24 的 24 个 step **全部**改善（h06 6/6、h01 1/1），符号检验 p≈6e-8，不是某一步走运；
2. 换估计器没用：h06 上 `cell_features` 与它的三个变体（深预算 0.5144、有序目标 0.5139、
   ExtraTrees 0.5144）挤在 0.0006 桩以内——**收益全部来自那 8 列特征，与用哪个树无关**；
3. 与我上一轮说的"唯一值得再跑的一件事"相反，**两种做法不叠加**：把先验查表同时做成特征和混合项
   （`cell_plus_prior`）相对只做特征毫无改进（h01 0.4866 vs 0.4867、h24 0.5238 vs 0.5241）。
   问题是同一个，治一次就够了；
4. 纯查表 `table_only` 在三跨度都**输给现行模型**（h01 0.5462、h06 0.5478、h24 0.5482），
   所以这不是"回归到统计基线"，模型仍带独立信息。

**为什么没有据此发 0.4.0**（这是范围判断，不是效果判断）：那 8 列是 (站×小时) 的历史统计，
线上必须**在线按 `reference_time` 之前的 TRAIN 窗口推导**，否则就是把未来写进特征——`predict.py`
与后端特征构造现在都没有这个口径，要新增一个先验表 profile 块、防泄漏测试和契约核对；
相对收益（−1.1% vs 现行服务版 0.5047）不足以单凭本轮就动服务侧。若组长要，我可以按第 5.9 节的
断点续训把它跑成正式批次，工作量在**在线特征口径**而不在训练。

### 5.8 「先训练 5 个 epoch」——树模型没有 epoch，实测换算（本轮新增）

交接文档里 B 线用的是 HistGradientBoosting：**一轮 = 一棵树，一个 bundle 里每个应答小时各训一个模型**，
早停（`n_iter_no_change=15`）通常在预算用完前就收工，所以"epoch"在这条线上没有对应物。
能对应"训练更久"的量是 boosting 预算，本轮把它做成显式开关 `--rounds-multiplier`：
预算与耐心**同时**放大（只放大 `max_iter` 而留着 15 棵的耐心，早停会在原处停下，等于没加），
学习率等其余参数一字不动，且产物改名（`avail-ord-h01-r5` / `modelVersion 0.2.0-r5`），
不允许一个拉长预算的模型顶着已发布身份进注册表。

实测（`ml_avail_run3_r5`，5× 预算 = 1250 棵、耐心 75 棵，全 12 个 bundle、同 seed 20260913，
**总用时 829.7 秒 ≈ 13.8 分钟**，记在 `train_summary.json` 的 `elapsedSeconds`）：

| bundle | 5× 实际用掉的轮数 / 预算 | 5× TEST MAE | 已发布 0.2.0（1×）TEST MAE | 差（正=变差） |
| --- | --- | --- | --- | --- |
| h01 | 189 / 1250 | 0.5291 | 0.5274 | +0.0017 |
| h06 | 200–308 / 1250 | 0.5654 | 0.5658 | −0.0004 |
| h24 | 199–346 / 1250 | 0.5791 | 0.5780 | +0.0011 |
| 9 个冷启动 bundle | 154–361 / 1250 | 0.5056–0.6176 | 0.5039–0.6192 | −0.0016 ~ +0.0049 |

三点读法：

1. **预算放开后早停仍然提前收工**：每步实际用掉 154–361 棵，只占 1250 棵预算的 12%–29%——
   "再训久一点"这件事模型自己投了反对票，它找不到还能减少验证损失的树；
2. 12 个 bundle **8 个变差、4 个变好**，最大一格 0.0049 桩（半个桩的 1%），量级完全在改点值规则
   （第 5.2 节，−8%~−9%）之下；
3. 与第 5.7 节第 2 点独立吻合：h06 深预算变体 VALIDATION 0.5144 vs 250 棵的 0.5145（实际用掉 162 棵）。
   **加深预算换不来分数，所以 5× 版不发**；开关留在代码里，产物名字自己会说明身份（`-r5`）。

对照资格说明：`ml_avail_run1` 与 `ml_avail_run3_r5` 是同一批导出行、同一划分、同一 seed、同一特征列，
唯一变量是预算；今天重跑的**默认**（1×）配方与 run1 逐位相同
（`outputs/ml_avail_recipe_check_v1/h01/model_metadata.json` 与 `ml_avail_run1/h01/model_metadata.json`
完全一致：`mae 0.5274 / rmse 0.7259 / n 18000`），所以上表的差值不是两版代码的差值。

### 5.9 断点续训（应组长要求，本轮新增）

两个长入口（`train`、`build_hierarchical`）本来就**每 bundle 落一次盘**（`train` 全量 12 次、
`build_hierarchical` 12 次），缺的只是"敢不敢接着用"。加 `--resume` 后：

- **守卫只放宽一半**：目录里已有 bundle 时允许续跑；已有 `train_summary.json` /
  `hierarchical_report.json` 时**照样拒绝**——那份汇总数字已经被引用过，续跑会把它变成混合产物。
  要新结果就换目录（`RunDirectoryGuardTest` 第 5 个用例锁住这条）。
- **复用要先验明配方**：`train` 比对 modelVersion / featureVersion / datasetId / 发布批次 ID /
  原始 manifest 哈希 / 特征列 / seed / 跨度 / 留出城市 / `roundsMultiplier` / steps；
  `build_hierarchical` 另比 `reusedEstimatorFrom` 与 `sourceModelId`，并从**基座 bundle** 推导期望值
  （不拿自己证明自己）。任一项不符直接退出并打印 `saved=... this command=...`。
- **复用不改一个字节**：bundle 的 `model.joblib` 先按 `model_metadata.json` 里的 sha256 复核，
  加载基座也走同一道校验；被复用的 bundle 在汇总里列进 `resumedBundles`，并额外记
  `reproducibleCommandNote` 说明"这一份是复用来的"。
- **表格不会被续跑改数**：`build_hierarchical` 现在把每个 bundle 在汇总里的**那一行**原样存进自己的
  `training_report.json`（`payloadEntry`），续跑读回来，而不是拿四位小数的 MAE 反推百分比
  （反推会把已发布的 `+0.57%` 变成 `+0.56%`）。更早、没写这个字段的已发布 run2 bundle 走重算路径，
  重算用的是报告里保留的全精度每小时 MAE，逐位等于当时发布的那一行——这条由
  `test_a_row_rebuilt_from_an_older_bundle_matches_the_published_one` 锁住。
- 半路断（bundle 有、报告缺）与文件被改动（哈希不符）都**不静默降级**，直接报错退出。

实测（`train` 一律是那条 5× 命令，只差 `--resume`）：

| 场景 | 用时 | 结果 |
| --- | --- | --- |
| 全 12 个 bundle 一口气跑完 → `ml_avail_run3_r5` | 829.7 s | 基准 |
| **同样命令，但目录里已经躺着 6 个 bundle** → `ml_avail_run4_r5_resume` | **380.7 s** | `[resume]` 复用 6 个（`model.joblib` 逐字节未动），重训缺的 6 个；**12 个 bundle 的 `metrics` 与 run3 逐位相同，连重训那 6 个的产物哈希也一样** —— 续跑与一口气跑完不可区分 |
| 已完成的 run（有 `train_summary.json`）上再 `--resume` | 1.2 s | 装载数据帧之前就拒绝：`already holds saved bundles or a train summary` |
| `train --horizons 1` 单 bundle 跑完再 `--resume` | 9.3 s → 1.9 s | `resumedBundles` 命中、`pooledTest` 逐位相同、产物哈希不变 |
| `build_hierarchical --only h01` 跑完再 `--resume` | 4.5 s → 2.1 s | `hierarchical_report.md` 除"生成耗时"一行外逐字符相同 |
| 配方不符（把 seed 改成 7 再续跑） | 装载数据帧后、任何写入前退出 | 打印 `seed saved=... this command=...`，checkpoint 未被触碰——这一格是 `ResumeEndToEndTest` 第一段在真批次上断言的（含"拒绝后 `model.joblib` 哈希不变"），不是手跑记录的 |

「重训出来的 6 个与一口气跑的 6 个连 pickle 字节都一样」这句值得单独说：它意味着 `--resume` 的
正确性检验可以做到"整目录比对"这种强度，也意味着 5× 预算下 sklearn 在这台机器上仍是确定的
（同 seed、同版本、同批次）。**这条只对本批产物成立**，换 numpy/scikit-learn 版本后不能拿它当前提。

守护用例：`ResumeGuardTest` 11 个（复用/拒绝的每种组合、半断、篡改、`published_content` 对不存在目录
不再炸）、`HierarchicalResumeTest` 8 个、`ResumeEndToEndTest` 2 个（真命令行 + 真批次，不用 stub）。

顺手修掉一个真实缺陷：`artifacts.bundle_directories()` 对**不存在**的目录会抛 `FileNotFoundError`，
而 `--output` 指着一个还没建的目录正是正常首发场景，旧守卫会当场崩；现在由 `published_content()`
先判 `is_dir()`，并有对应用例。

### 5.10 预测表（应组长要求「把预测表发给我看看」，本轮新增）

`forecast_table.py` 用**当前出厂的服务 bundle**（重绑后是 `ml_avail_run6` / 0.3.0；`--run-dir` 的默认值
就取自 `__init__.py` 的 `HIERARCHY_RUN`，不再写死在源码里）走真实推理路径
（`AvailabilityForecaster.predict()`，不是重新实现一遍），对 25 个站在 6 个参考时刻上各出一张表，
写到 `outputs/ml_avail_forecasts_v2/`（上一批的是 `ml_avail_forecasts_v1`，两份 `forecasts.csv`
**sha256 相同**：`b2a02fedbad3286d14d636444cc9536d802193798c9e8be1d90e8cf60e248aeb`，见第 5.12 节第 4 点）：

- `forecasts.csv`：4,650 行，每行 = 一个 bundle × 一站 × 一个参考时刻 × 一个未来小时；列含对外的整数
  `predicted_chargers`、区间 `interval_low/high`、预计值 `expected_chargers`、`p_no_charger`、完整分布
  `distribution`，以及对照用的 `actual_chargers` / `abs_error` / `within_interval`；
- `forecast_table.md`：窗口偏差表、分站误差表、单站 24 小时曲线、站×小时的预测/实际对照网格。

这一段窗口上预测与真实值的平均绝对偏差：h01 0.4333（150 点）、h06 0.3533（900 点）、
h24 0.5011（3,600 点）桩。**这几格不是模型准确率**——样本只有几百到几千点，且只覆盖表头列出的那几个
参考时刻；交付口径的准确率是第 5.1 节全 TEST（18,000 / 107,250 / 418,200 点）上的
0.4816 / 0.5047 / 0.5120。表的作用是**逐行核对预测合法不合法、离不离谱**，两个口径不要混。
表里出现的都是整数桩数，小数只在 `risk()` 的预计值侧信道（第 4 节）。

### 5.11 测试执行记录（`outputs/` 不进 git，故把跑测结果也抄在这里）

```
$ python -m unittest discover -s data_analysis/ml/tests -t .
Ran 65 tests in 139.041s
OK          # 退出码 0
```

（这次跑在 rebase **之前**：那时仓库里的 `data_analysis/datasets/analytics_full_180d_v1/` 还是
`067f7b9` 版导出，批次号与我的产物一致；rebase 之后的记录见下一段。）

环境：Python 3.13.9 / scikit-learn 1.7.2 / pandas 2.3.3 / numpy 2.4.4（与 `model_metadata.json`
的 `dependencies` 同一台机器同一环境）。

**rebase 到 `origin/develop`（`d8b5fa2`）之后同一条命令的记录，本轮实测，如实抄在这里**：

```
$ python -m unittest discover -s data_analysis/ml/tests -t .
Ran 65 tests in 741.462s
FAILED (failures=38, errors=7)      # 退出码 1
```

45 个红项逐条归类（原因只有一条，见第 2.1 节）：

| 用例 | 红项数 | 报错原文（截断） |
| --- | --- | --- |
| `ShippedBundleTest.test_bundle_is_bound_to_the_batch_it_reads` | 24 | `AssertionError: 'analytics-5f8e…' != 'analytics-298a…'` |
| `ShippedBundleTest.test_prediction_satisfies_the_public_contract` | 6 | `PredictionError: the served batch differs from the trained batch` |
| `ShippedBundleTest.test_a_request_cannot_smuggle_the_answer_in_as_history` | 1 | 同上（防泄漏那条也跑不到断言就抛了） |
| `MinimalInferenceTest.test_every_shipped_bundle_passes_or_is_explicitly_skipped` | 6 + 1 | 前 6 项同上；那 1 项是聚合断言 `an ordinary 1/6/24-hour bundle is missing`，因为已经没有一个 PASS |
| `MinimalInferenceTest.test_served_values_are_whole_chargers_within_the_station_capacity` | 6 | 同上 |
| `MinimalInferenceTest.test_the_self_check_command_exits_zero_on_a_shipped_bundle` | 1 | 命令退出码 `1 != 0`，输出 `[self-check] 0 passed, 1 failed, 0 skipped` |

合计 24 + 6 + 1 + (6+1) + 6 + 1 = **45**，与上面的 `failures=38, errors=7` 对得上（errors 是
`ShippedBundleTest` 里那 7 个抛异常的子项）。

**12 个测试类里只有 `ShippedBundleTest` 与 `MinimalInferenceTest` 两个变红**（两个都在"读已发布
bundle"这条路上），其余 10 个类仍为绿——含本轮新加的续训、boosting 预算、特征变换、先验收缩、
数据绑定与审计脚本类，也包括在合成小帧上跑的真训练端到端用例。
741.5 s 与上面 139.0 s 的差**没有归因**——本机当时并非我记录的"空闲"状态，这条只按实测抄两个数，
不解释成用例变贵（上一段那次更正的教训同样适用）。

**上一版这里写的 `Ran 36 tests in 768.718s` 是失真的，本轮更正**：那次测量时同机还挂着三个模型筛选
任务在抢 CPU（第 5.7 节的 h01/h06/h24 检索），不是这套用例的代价。同一台机器、空闲时的三次记录是：
36 个用例（上一版）→ 52 个用例 **108.2 s** → 现在 65 个用例 **139.0 s**。新增的 13 个里有两个
"真训练 + 真续跑"的端到端用例（`ResumeEndToEndTest`），代价主要在反复装载 107,425 行的批次上，
所以 +13 个用例 ≈ +31 s。这条更正同时改掉第 7 节第 2 条里"这个 job 太贵"的暗示：分钟级；缺科学栈时
整套 skip（测试文件开头的 `_depends_available()` 守卫），缺导出批次时只有绑数据的那几个类 skip，
所以新加的这个 CI job 在没有数据的 job 上也不会变红。

67 个用例按主题分布（`ml/tests/test_ml_contract.py`；上面那两次跑测分别是 65 与 65 个用例，
`RunPointerTest` 的 2 个是 rebase 之后为第 2.1 节那条失效路径新增的，故尚未出现在旧记录里）：

| 测试类 | 个数 | 锁住的是什么（按用例名如实列，不夸大） |
| --- | --- | --- |
| `ShippedBundleTest` | 6 | bundle 与其读取的批次互相绑定；对外结果过 `validate_prediction`；层级 bundle 用的是自己那份先验；站点档案保持在线构建器能读的数值块；在线行能被档案完全填满；请求不能把答案当历史偷渡进来 |
| `PriorShrinkageTest` | 8 | 查表单元按"它描述的小时"而非"它被写下的时刻"取键；支持度只数 TRAIN 的小时；历史越多越偏向查表（run1 老报告把这个方向写反了）；每级用自己的伪计数；没见过的行退回模型；借来的单元按缺失的那个站池化；并列分布上中数优于众数；混合结果仍是合法 pmf 且能取到端点 |
| `FeatureTransformTest` | 5 | 在线特征与离线导出逐列一致；滞后窗口止于第一个应答小时前一刻；历史不完整/非法即拒；标签=应答小时最后采样时刻的空闲桩数；标签列与划分列不得出现在输入里 |
| `MinimalInferenceTest` | 4 | `--self-check` 下每个已发布 bundle 要么 PASS 要么被显式 SKIPPED；对外值是 `0..capacity` 的整数（小数只走 `risk()`）；改掉 `artifactSha256` 必须变 FAIL；命令本身退出码为 0 |
| `RunDirectoryGuardTest` | 5 | `train` 拒绝已放 bundle 的目录；拒绝"写了一半"的 run；拒绝把 `--resume` 用在一份已完成的汇总上；`build_hierarchical` 同样拒绝有内容的目录；`prepare_data` 先写的 `data_profile.json` 不算已发布（否则正常流程会被自己的守卫挡住） |
| `ResumeGuardTest` | 11 | 配方一致的 checkpoint 原样复用（哈希不变、`boostingRounds` 形状齐）；没存过就返回"没有"；seed / 批次+manifest / `roundsMultiplier`+modelVersion / 跨度+留出城市 / steps 任一不符即拒；半断（报告缺）与哈希不符各自报错；`published_content` 对不存在的目录返回空（旧实现在这里抛 `FileNotFoundError`）；`data_profile.json` 不计入、bundle 与汇总计入 |
| `HierarchicalResumeTest` | 8 | 出厂 0.3.0 产物（无 seed 字段）不被冒领；补上 seed 才复用；从别的基座/别的批次包的直接拒；`payloadEntry` 存在时**原样读回**而非重算；不存在时按报告里全精度每小时 MAE 重算，逐位等于当时发布的那一行；续跑的 `hierarchical_report.md` 与已发布版逐字符相同；只跑主城的部分 run 不印 `nan`。**fix 之后仍钉在 run1 / run2 这两份旧产物上**（本轮数过磁盘上全部 7 个 `ml_avail_run*`：`seed` 缺失的只有
run2，`payloadEntry` 只有 run6 有——那字段是 `build_hierarchical` 写的，run1/run2 都没有，换任何新目录都
没东西可测），故加了 `requires_legacy_runs` 守卫：那两份不在磁盘上时整类 skip 并说明原因，而不是假装通过 |
| `RunPointerTest` | 2 | 本轮新增（第 5.12 节）：测试里的 `RUN_NAMES` 必须与模块常量 `BASE_RUN` / `HIERARCHY_RUN` 一字不差；每个权威 run 的 `h01/model_metadata.json` 里 `trainingPublishedBatchId` 必须等于仓库当前导出的批次号，不等就报错并直接给出下一步（"retrain into a new directory and repoint …"）。两条边界：两个 run 都不在磁盘上才 skip（空仓库clone没有可声称的绑定）；只有一个在 = 重绑做到一半，**报错并列出缺哪个**，否则 `HIERARCHY_RUN` 指着不存在的目录这件事就永远绿着过去。这三条我都用改名过的 `RUN_DIRS` 反向跑过一遍（第 5.12 节第 7 点）。数据层再重发布时，套件红在这两条上，而不是像这次红 45 项让人自己猜 |
| `ResumeEndToEndTest` | 2 | 真命令行 + 真批次：断掉的 train 续跑后 `pooledTest` 与分数逐位相同、`model.joblib` 哈希不变、改 seed 的续跑被拒且没碰 checkpoint；断掉的先验包装跑后表格除"生成耗时"一行外逐字符相同 |
| `BoostingBudgetTest` | 7 | 已发布配方在代码里钉死；×1 与钉死值一字不差；×5 同时放大耐心（1250/75）且不动学习率；×0.4 取整为 100/6；0 与负数拒绝；`-r5` 改名；包一层时继承基座的 `-r5` 名字 |
| `DataBindingTest` | 3 | 导出必须绑到原始包 manifest（成员 A 旧产物正是把 serving manifest 当成了它）；无法核验的导出直接拒绝；`ml_targets_hourly` 永不进服务库 |
| `AuditScriptTest` | 3 | 审计脚本自己的收缩方向与取键口径；已发布报告不被就地改写 |
| `InvocationRecordTest` | 3 | `-m` 运行记录模块名而不是 `__main__`；直接按文件路径运行时记录路径；命令行参数逐字记录 |

**重绑之后同一条命令的记录（本轮实测，套件回到绿；下面抄的是提交版本那一次的原文输出）**：

```
$ python -m unittest discover -s data_analysis/ml/tests -t .
Ran 67 tests in 707.000s
OK          # 退出码 0
```

同一份套件本轮一共跑了四次，逐个抄、不挑好看的：**928.305 s**（重绑当轮，含我自己在同机并发跑的核对
探针——反复装载 107,425 行训练帧、给合计约 1.4 GB 的 24 份 `model.joblib` 算 sha256）→ **824.965 s**
（自评审改完测试代码之后，同机还有另一路只读核对在跑它自己的探针）→ **707.000 s**（提交前那一份代码，
本轮唯一一次我这侧没有并发探针）→ **1110.534 s**（第 5.13 节那次 rebase 之后，即与成员 A 负荷线合流的
同一份树上重跑）。四次都 `OK`、退出码 0、输出里都没有 `skipped=`。

67 = 65 + `RunPointerTest` 的 2 个。输出里没有 `skipped=` 字样 ⇒ **67 项全部真跑**：既包含读
`ml_avail_run5` / `ml_avail_run6` 的绑定与推理用例（说明重绑后的产物真的过核对），也包含被
`requires_legacy_runs` 守卫的 `HierarchicalResumeTest`（run1 / run2 还在磁盘上，所以那 8 项旧格式回归
也真跑了，没有被 skip 蒙过去）。这三个数都**不能**与上面空闲时的 139.0 s 比较，也不能与上面那次红的
741.5 s 互相换算——这台机器整轮都开着 IDE / 微信 / 虚拟机，它们只是"这一轮跑过的分钟数"。
新增的那个类干什么用的，写在上面的分类表 `RunPointerTest` 那一行与第 5.12 节第 6、7 点。

### 5.12 按新批次重绑产物（应组长「那你就根据问题来改正」，本轮实测）

**要改的问题**就是第 2.1 节：上游 PR #64 把导出重发布了一次，`publishedBatchId` 换成
`analytics-298aa3ee…`，`predict.py` 的批次核对因此拒收我全部已发布 bundle，套件红 45 项。
正解只有一条——**按新批次重跑一遍再发**。没有改测试里的批次号、没有放开核对、没有把旧目录就地改写。

**跑的第 3 节命令（配方一字未动：同 seed 20260913、同 `--rounds-multiplier 1`、同三个留出城市、同 75 列）**，
各步自计耗时如实抄在这里：

| 步骤 | 新目录 | 自计耗时 |
| --- | --- | --- |
| 1) `train` 全 12 个 bundle | `ml_avail_run5` | **5191.8 s**（`train_summary.json`） |
| 2) `build_hierarchical` | `ml_avail_run6` | **551.0 s**（`hierarchical_report.json`） |
| 3) `evaluate` | `ml_avail_eval_run6_v1` | **197.8 s**（报告头） |
| 4) `predict --self-check` | 不写文件 | 3 PASS / 0 FAIL / 9 SKIPPED（原文见附录 B） |
| 6) `forecast_table` | `ml_avail_forecasts_v2` | 脚本不自计耗时，故不给秒数 |

第 0 步（可选的 `prepare_data` 画像）**本轮没重跑**，`ml_avail_run5/run6` 里没有 `data_profile.json`。
不影响可追溯性：逐分片 sha256 校验在 `data_io._read_shards()` 里，**每次装载某张表时那张表的每个分片都要过**
（不符即 `ExportError`），本轮 train / build / evaluate / forecast_table 四次装载路径全部通过；
`prepare_data` 只是把同一件事的结果另存一份画像。
第 5 步（单站单次推理的命令行例子）本轮也**没有手工重敲**——它不是发布步骤，服务路径由第 4 步的
`--self-check` 与 `MinimalInferenceTest` 覆盖，而这两条本轮都是绿的。
这些数字**不能**与第 5.8 节空闲机器上测的 829.7 s（那是 5× 预算）或本节下面的套件时间互相换算——
本轮同机一直有 IDE / 微信 / 虚拟机在跑，第 5.11 节那次"不解释成用例变贵"的教训同样适用。

**旧目录一字未动**（`ml_avail_run1` ~ `ml_avail_run4_r5_resume` 仍在磁盘上），第 5.11 节那条红记录描述的
就是它们当时的状态。

**"重绑只是元数据事件"这句话本轮是被证出来的，不是被预期的**（探针 `probe_rebind.py` /
`probe_payload.py` / `probe_trees.py` / `probe_reports_diff.py`，均临时脚本、不入库）。全部指标为模拟数据
测试结果（第二阶段发布批次），不代表真实运营数据表现：

1. **24/24 个 bundle 的 `model_metadata.json` 分数块逐位相同**：`mae` / `rmse` / `testSamples` 三项，
   run1↔run5 与 run2↔run6 各 12 份，一个字节都没动（例：h01 `0.5274 / 0.7259 / 18000`，
   出厂 h01 `0.4816 / 0.7223 / 18000`，冷启动 9 份同样）。
2. **出厂层的"选择"也复现了**：12 个 bundle 的 `pseudoCount`（每层 k）、`priorWeightMean`、
   `pointRule`（全部 `median`）、`testModelAlone`、`testModelWithChosenRule`、`validation`、`splits`
   全部相等。`hierarchical_report.json` 的 17 个字段里只有两个变了：`reusedEstimatorFrom`（12/12，
   `ml_avail_run1/…` → `ml_avail_run5/…`）和 `reused`（旧版这个键不存在，新版显式 `false`）。
   **`directory` 没变**——它是相对名（`h01` / `coldstart_DL/h06`），这也是 `HierarchicalResumeTest`
   能跨批次直接比它的原因（`test_ml_contract.py:970` 断言 `entry["directory"] == "h01"`）；上一版这里
   把"`reusedEstimatorFrom` 里含目录名"写成了"`directory` 也变了"，本轮按字段逐个数过，改掉了。
   先验是在 VALIDATION 上选的，VALIDATION 一行未变，所以选择不可能变，实测确认它没变。
3. **评估报告 125 行 → 125 行，只有 6 行被替换**（`unified_diff` 里显示为 12 行 = 6 对 `-`/`+`），
   全在头部：数据集/批次行、被评分目录行、导出清单
   sha（`358eeba3…` → `7e0e5066…`）、生成时间与耗时、复现命令、以及那句"本报告写入新目录"的路径。
   **所有分数行、分城市表、冷启动表、风险阈值表逐字符不变**——所以第 5.1~5.6 节的每张表在新 run 上
   原样成立，不必重抄数字。
4. **预测表 `forecasts.csv` 两份 sha256 完全相同**（`b2a02fed…`，4,650 行 + 表头）；
   `forecast_table.md` 107 → 108 行，多的一行是本轮新加的「来源 run / 批次」头，另有两处是我把
   写死的 run 名改成从产物里读出来的表头与"看该 run 的 `evaluation_report.md`"（见下面第 6 点）。
5. **`model.joblib` 的字节确实变了，24/24 全变**——这一点上一版报告里我按"预期哈希不变"来设计探针，
   实测把它证伪了，如实记下原因：批次号是**打在 pickle 里**的（`bundle["publishedBatchId"]`，出厂包另有
   `inheritedFrom`），所以换批次必然换哈希；此外新代码在 bundle 里多写了 `roundsMultiplier`、基座模型多写
   `point_rule`、报告多写 `reused: false`，`training_report.json` 的 `perStep.*` 多了
   `boostingRounds` / `maxBoostingRounds` / `calibration.{pointRule,validationMae,modelMae}`、
   `pooledTest` 多了 `maeExpectation`（全是"字段以前不存在"，不是"值变了"）。
   `model_metadata.json` 里对应的 `artifactSha256` 24 份也全部跟着变——**如果哪里钉过这个哈希，必须更新**
   （`MinimalInferenceTest` 的变异用例正是为这件事存在的）。
   唯一一处我**没能**归因到字段的技术细节：h01 基座估计器的 pickle 比旧的多 23 字节，而它 44 个属性逐个
   比较全部相等、129 轮树的 1,548 个数组/标量按值全部相等、18,000 行 TEST 的 4 类 pmf 与中位数
   **逐位相同**（h06/h24 的估计器字节反而完全相同）。也就是说这是编码/对象图层面的差异，不是模型状态
   差异；我说"说不出差在哪个字节"，因为按值找不到它。
   **这个 23 字节本轮换了三把尺子重测**（自评审时另一路核对把它读成"新产物少 23 字节"，那是把 run1 和
   run5 的数字对调了，实测如下，方向以本表为准）：单独 `pickle.dumps(h01 估计器)`——协议 5（Python 3.13
   默认）run1 1,889,978 → run5 1,890,001 = **+23**，协议 4 同为 +23，协议 2/3 为 **+37**；
   `joblib.dump(compress=3)` +30、`compress=0` +32；整份 `model.joblib` 文件
   1,965,175 → 1,965,223 = **+48**（其中还含新写的 `roundsMultiplier` 键），出厂包
   2,017,083 → 2,017,119 = **+36**。所以"多 23 字节"这句只对"协议 4/5 单独 pickle 估计器"这一个口径成立，
   换成量文件就不是这个数——报告里从此把尺子一起写出来。
6. **下次同样的事会先红在哪**（本轮新增 `RunPointerTest`，2 个用例）：① 测试里的 `RUN_NAMES` 必须等于
   `ml/availability/__init__.py` 的 `BASE_RUN` / `HIERARCHY_RUN`；② 两个权威 run 的 `h01` 元数据里
   `trainingPublishedBatchId` 必须等于仓库当前导出的批次号，不等就直接说"retrain into a new directory and
   repoint"。数据层再重发布一次，套件红在这两条上并给出下一步，而不是像这次红 45 项让人自己猜原因。
7. **这条守卫被反向验过**（自评审时补的，探针 `probe_pointer_negative.py`，临时脚本、不入库）：只在今晚
   这种"磁盘正好是对的"状态下变绿的守卫等于没有，所以我把它读的 `RUN_DIRS` 换成四种状态，各跑一次真实
   用例——
   * 换成被取代的 `run1` / `run2` ⇒ `FAILED (failures=2)`，消息里既有
     `'analytics-5f8e9342…' != 'analytics-298aa3ee…'` 也有下一步建议；
   * 换成"基座在、出厂目录不在"（重绑做到一半）⇒ `FAILED (failures=1)`，消息列出
     `present: ['ml_avail_run5']` 与 `missing: […]`，不会像上一版那样让 `subTest` 之外的路径静默跳过；
   * 两个都不在 ⇒ `OK (skipped=1)`，理由写明"none of ml_avail_run5 / ml_avail_run6 is trained"——
     刚从 git clone 出来、没跑过管线的机器不该红；
   * 换回真正的两个目录 ⇒ `OK`。
   第 ② 条这一版还改了跳过口径（上一版是"逐目录 `skipTest`"，一半存在时另一半会被静默跳过），
   第 5.11 节的绿记录是按改完之后的代码重跑的。
8. **本节上面这些数字又被第二路独立核对重算过一遍**（同一台机器、同一个终端会话之外的一份只读评审，
   它自己写探针、不读我的结论）。它给出的 8 条里：
   * 站得住并已改的 3 条——`directory` 其实没变（第 2 点已按字段重述）、run2 的 `seed` 是**键不存在**
     而不是 `null`（第 3.1 节已改，并把 `HierarchicalResumeTest` 的断言从 `assertIsNone(get(...))`
     换成 `assertNotIn`，让代码真的钉住报告说的那件事）、"新产物都没有 `payloadEntry`"这句在
     `test_ml_contract.py` / `availability/__init__.py` 的注释里写宽了（实测只有 run6 有、只有 run2 缺
     `seed`，注释已按测量收窄）；
   * 复核后**维持原文**的 2 条——它说 h01 估计器是"少 23 字节"（把我两个操作数对调了，第 5 点现在把
     四把尺子都列出来）、说第 2.1 节的 45 项拆分与第 5.11 节差 6 项（它漏算了 `test_served_values_…`
     那 6 项：24 直接断言 + 19 抛 `BATCH_MISMATCH` + 1 聚合 + 1 退出码 = 45，两处口径本来就一致）；
   * 剩下 3 条在我这一版工作树里已经先改掉了（"12 行"→6 行被替换、源码行数 3,260/31/900/1,195、
     第 7 节第 2 条的"65 个用例"）。
   记这一条是为了口径：本节的每个数字要么自己量过，要么被两把不同的尺子量过。

**版本号没有升**（考虑过 `0.2.1` / `0.3.1`，否决并记在这里）：已发布指标块仍只有
`mae/rmse/testSamples/unit`，`modelId` 不变，而"是哪一批次训出来的"本来就有硬判别器——
`predict.py` 的批次核对会直接拒收错配产物（本轮第 2.1 节就是它的行为演示），升版本只会让第 5 节所有
引用与 `contracts` 口径的文档多改一遍。真要改，改的是 `__init__.py` 里两个常量。

**跑完的结果**：`Ran 67 tests in 707.000s` / `OK`（退出码 0）——这是按第 7 点改完测试代码之后再跑的那一次，
本轮另两次（928.305 s / 824.965 s）与时间口径的免责声明都在第 5.11 节最后一段——那台机器整轮开着
IDE / 微信 / 虚拟机，前两次还叠了我自己的核对探针，所以这些秒数只当"这一轮跑过的分钟数"用。

### 5.13 第二次 rebase：与成员 A 的负荷线合流（本轮实测，批次未变）

开 PR 之前 develop 又前进了 13 个提交——全是成员 A 的负荷线（PR #66，merge 提交 `10da4cc`）。
按组长指示把本分支 rebase 上去（改写已推送的历史之前先建了备份分支
`backup/phase2-ml-load-prebase-20260914`，旧 tip `7d9f955` 仍在里面）。

**这次合流不动批次**，三条独立的证据：

1. 自 `d8b5fa2`（我上一轮 rebase 的落点）起，develop 只新增 **18 个文件**：17 个在
   `data_analysis/ml/load/`，1 个 `data_analysis/tests/test_ml_load_review_fixes.py`。
   没有一个碰 `data_analysis/datasets`、`data_analysis/docs`、`contracts/`、`backend/`，
   也没有碰我的 `ml/common/` ⇒ 导出批次仍是 `analytics-298aa3ee…`。
2. 判别不是"看过 diff 觉得没事"，而是第 5.12 节第 6 点那条守卫在跑：rebase 之后 `RunPointerTest`
   两项仍绿，核对的就是"权威 run 绑的批次 == 仓库当前导出的批次"。全套 67 项 `OK`（1110.534 s，
   第 5.11 节第 4 次），成员 A 那 22 项在同一份树 `0.917 s / OK`。
3. 我这一侧内容逐字节未变：`git diff backup/… HEAD -- ml/availability ml/common ml/tests
   ml/__init__.py ml/requirements-draft.txt` 输出为空。

顺带两条给组长的观察（不改，只记）：A 线自己另写了一份 `ml/load/common.py`（没复用 `ml/common/`），
且把回归测试放在共享的 `data_analysis/tests/` 而不是 `data_analysis/ml/tests/`；
他们的测试与我的一样用 `try/except ImportError` 兜住"CI 没装科学栈"，也就是第 7 节第 2 条那个缺口
现在**两条线都感受到了**。

## 6. 独立扩展：目前未做，以及做之前需要什么



按文档「时间不足时先交付基础预测闭环」，本轮把基础线的缺口（复现命令入库、最小推理测试、
交付说明）补完，未开新任务线。择一建议：

- **推荐：会话级异常筛查（新 `ml/screening/`）**。素材已在仓库 raw 批次里：
  `battery_samples` 1,217,829 行、`charger_telemetry` 3,888,000 行、`charging_sessions` 123,456 行、
  `charging_attempts` 169,154 行、`vehicle_energy_intervals` 169,254 行、`anomaly_labels` 4,865 行。
  致命陷阱必须先说清：`charging_sessions.stop_reason` 近似标签本身（`USER_STOPPED → EARLY_STOP`
  精确率 98.9%），`anomaly_labels` 只能进**评价**不能进特征；因此特征只能取自会话**中途**的
  telemetry/电池窗口。此外 raw 批次没有 clean Parquet，需要 Linux 成员给出已验收的清洗批次号，
  否则第 3 天拿不到可训数据。
- 偏好推荐 / 结合预测的站点选择：依赖文档第 6 页所列的曝光点击与路网数据准备，当前批次不具备，
  换名字不解决问题。
- 若组长批准开新任务：按文档要求**独立准备数据、新增契约**，不会强塞进
  `/predict/availability` 的接口。

## 7. 需要组长拍板的事（我只提案，不动共享文件）

1. **分支名**：`feature/phase2-ml-load` 内容与名字不符（实为 B 的可用性线）。**本轮已按组长要求
   rebase 到 `origin/develop` 并推送了这个名字**，所以改名现在要多花两步：本地
   `git branch -m feature/phase2-ml-availability` → 推新名 → 删远端旧分支（PR 若已开需改 base 引用）。
   现在改仍然便宜，合入之后再改就麻烦了。另外这轮 fetch 看到远端**新增**了
   `origin/feature/ml-load-forecast`（不在 develop 里，看名字像是成员 A 的负荷线）——若确实如此，
   B 的分支叫 `ml-load` 就更该改了，两条线会撞同一个词。
   另外 `data_analysis/ml/PLAN.md`（仍未跟踪）是一份以**成员 A 负荷线**为主角的调研稿，与 B 的交付无关，
   请定：删掉、移交成员 A，还是标注为"A 线调研（由 B 的会话代查）"。
2. **CI 不跑 ml 测试，也没有任何地方声明 ML 依赖**：`.github/workflows/data-analysis.yml` 只有
   `generator` / `api` / `spark` 三个 job（已逐个看过），**没有一个跑 `data_analysis/ml/tests`**；
   仓库现有的三个清单（`data_analysis/requirements-api.txt`、`-spark.txt`、`-contract-test.txt`）
   **都不含 numpy / pandas / scikit-learn / joblib**（已逐个 grep 确认）。也就是说本线代码的运行环境
   目前只写在每个产物的 `model_metadata.json.dependencies` 里，从仓库里**装不出来**，
   换一台干净机器照第 3 节敲命令会在 `import pandas` 就断。
   这 67 个用例是本次全部"机械核查"的落点，建议加一个装科学计算包、跑
   `python -m unittest discover -s data_analysis/ml/tests -t .` 的 job，并新增一份
   `data_analysis/requirements-ml.txt`（**此文件尚不存在**）。CI workflow 与 requirements 都是共享文件，
   我不改，只提案；依赖清单草案已落在 **我线目录内**：`data_analysis/ml/requirements-draft.txt`
   （钉住本轮真实环境 numpy 2.4.4 / pandas 2.3.3 / scikit-learn 1.7.2 / joblib 1.5.2，
   并写明"3.11 能否加载本批 pickle 未实测"这条风险），组长认可后可直接 `mv` 到 `data_analysis/` 下改名采用。
   **耗时口径本轮更正**（见 5.11）：上一版记的 769 s 是三个模型筛选任务同机抢 CPU 时测的，
   空机上同一套用例是**分钟级**（65 个 139.0 s），且缺科学栈时整套 skip、缺导出批次时只跳过绑数据的
   几个类，所以这个 job 进得起 CI。
3. **模型注册入口缺失**：`backend` 目前无 `MODEL_REGISTRY` / `ANALYTICS_MODELS_DIR`，
   `tests/test_analytics_api.py` 断言两个 `/predict/*` 必须 503。我按文档要求交付推理代码
   （`AvailabilityForecaster` + 错误码），接入 `service.py` 的动作留给组长；需要组长确认
   注册目录约定与"加载失败仍回 503"的行为。
4. **是否重跑一遍模型产物**（**这条也已被第 5 条与第 5.12 节消解**：现在绑新批次的 24 份
   `training_report.json` 每份都带 `reproducibleCommand` 与 `seed`）：当时评估报告已经自带复现命令
   （`ml_avail_eval_run2_v2`），但**旧模型的 bundle `training_report.json` 仍缺 `reproducibleCommand`**
   （那一轮递归读过 run1/run2 的 24 份确认，详见 3.1），
   因为 run1/run2 先于这次改动。分两种花法：
   * 只花 **90 秒**重跑 `build_hierarchical`（run2 自己记的是 88.3 s；本轮单个 `--only h01` 实测 4.5 s，
     新目录、分数应逐位相同）→ bundle 带上**构建命令**，但训练命令仍进不了产物；
   * 花 **十几分钟**重跑 `train` + build + evaluate（新目录；`ml_avail_run3_r5` / `ml_avail_run4_r5_resume`
     已被第 5.8/5.9 节的预算实验占用，下一份正式产物请用 `ml_avail_run5` 之类的新名字）→ 每个 bundle 才真正
     自带训练命令与 seed。代价已实测：全 12 个 bundle 在 **5× 预算**下 **829.7 s**，默认 1× 只会更快。
     `HistGradientBoosting` 只在同版本、同 seed 下确定性——本轮这条从"预期"升级成"实测"：
     `ml_avail_run4_r5_resume` 里 6 个**重训**的 bundle 与 `ml_avail_run3_r5` 一口气跑出来的那 6 个
     连 `model.joblib` 哈希都逐位一致（同 seed、同 sklearn 版本、同批次）。真要重跑，跑完仍需按新目录
     重发一次评估报告（第 3 节第 3 步），不能拿旧报告的分数挂新 bundle。
   我倾向：**先不重跑**，等接口/注册口径定了再一次性出重跑版，免得同一批产物出现第三个版本号；
   但这条由组长定。（第 5 条已使这条倾向失效，保留是为了不掩盖我当时怎么判断的。）
   **本轮两处口径要更正**（重绑真的跑了一遍，见第 5.12 节）：① 上面"默认 1× 只会更快"是拿 5× 的空机
   实测外推的，本轮 1× 在**被别的程序压着的机器**上实测 **5191.8 s**，两个数字不同工况、不可比，
   所以"更快"当时就不该写；② "跑完仍需重发评估报告"这条照做了，另外还多了两个连带结果——
   24 份产物的 `artifactSha256` 全部变了（批次号打在 pickle 里），以及出厂层的 k / 点值规则
   复现到了逐位相同。
5. **是否按新批次重绑：本轮已执行，选的是"重跑再发"（组长「那你就根据问题来改正」）**。产物、评估报告、
   预测表全部换成绑 `analytics-298aa3ee…` 的新目录，套件 `Ran 67 tests … OK`；分数与绑定前的对照证据
   在第 5.12 节。**留给组长的只剩口径问题，不再是"要不要重跑"**：数据层每次重发布都换 `batchId` ⇒
   下游 ML 产物按纪律全部重绑，这条是不是验收想要的答案？若是，**成员 A 的负荷线产物同样要重绑**——
   本轮数过它留在磁盘上的三份 `outputs/ml_load_run1/*/model_metadata.json`：
   `trainingPublishedBatchId` 全是 `analytics-5f8e9342…`（已被 PR #64 取代），
   `sourceManifestSha256` 全是 `358eeba3…`（附录 A 第 2 条已经指出这本来就该是原始包 manifest
   `1f03e37c…`），也就是说 A 线产物同时踩着"绑错 manifest"和"绑到旧批次"两件事，
   它的 `modelId`/`metrics` 我只读不动，重训由 A 自己跑。这不该由 B 单方面决定，也不该由 B 代跑。
   本轮同时把"下次会先红在哪"补成了机制（`RunPointerTest`，第 5.12 节第 6 点），
   而不是靠人记得。**我没有为了让测试变绿去改测试里的批次号**——那条断言存在的意义就是挡这种事。
6. **`data_analysis/docs/data_layer_validation.json` 还钉在被取代的那一批上**（共享文件，B 不动，只报）：
   本轮自评审时顺手对了一遍 `data_analysis/docs/` 四份验证记录，`dataset_delivery_validation.json`、
   `cleaning_acceptance_validation.json`、`mysql_migration_validation.json` 三份的 `publishedBatchId`
   都已被 `2e0c35b`（2026-09-13 "refresh 180-day cleaned delivery and audit bundle"）刷成
   `analytics-298aa3ee…`，**只有 `data_layer_validation.json` 没有**——它第 24/25 行仍是
   `publishedBatchId analytics-5f8e9342…` 与 `pipelineRunId spark-62543990…`（旧批次），
   最后一次被改动是 `203bbec`（2026-09-12），早于那次刷新。它同一块里的
   `databaseSha256 4bc352ae…`（`databaseBytes 78,721,024`）因此也无从核对：
   `dataset_delivery_validation.json` 对**同样 78,721,024 字节**的库记的是另一个哈希 `820780c4…`，
   而工作树里现在没有任何 `.db` / `.sqlite` 文件（`**/*.{db,sqlite,sqlite3}` 零命中），
   我本地两把哈希都复算不了。影响面：谁按这份记录验收"数据层与当前批次一致"，会得到一个假阳性。
   请定：由数据层自己补一次刷新（推荐，B 不碰），还是这份记录被有意当作 09-12 那次验收的历史快照
   ——若是后者，建议在文件里加一个说明字段，否则与漏改无法区分。

## 8. 已知局限（不许被汇报口径抹掉）

- 数据是模拟生成的合成批次（`source: SIMULATED`），标签与特征出自同一套生成规则，
  因此 MAE 低不代表真实运营可复现；一切结论仅限本批次。
- 每站恰好 3 桩（`capacity` 只有 3 一个取值），所以 0..3 的有序分类是"恰好够用"；
  真实网络里 40+ 桩的场站需要改成"分布 + 归一化占用率"，届时合法率与 MAE 口径都要重定义。
- 冷启动先验无稳定收益（5.5）；小数期望值与整数库存的区分靠命名与文案，页面若把
  `expectedChargers` 当库存显示就是接入侧的错，接口已明确标注。
- 报告本体在 gitignored 的 `data_analysis/outputs/` 下，评审看不到——所以关键数字抄进本文件；
  产物本体（`model.joblib`，h24 约 59 MB）按文档要求不入 Git，走约定交付目录。

## 附录 A：给成员 A 的负荷线核查记录（不是 B 的交付物）

B 的会话在本分支上短暂重写过 `ml/load/`，用来验证共享数据管道，现已移出工作树（源码与
`__pycache__` 未混淆：`ml/load/__pycache__/*.pyc` 全程未动）。留下三条已实测的结论：

1. **推理锚点已复现（本轮实测）**：用 `ml/common/` 的读取与特征管道加载 `outputs/ml_load_run1` 的旧产物
   重算 TEST，得到 MAE **12.6107 / 13.6321 / 13.8804 kW**、RMSE **18.4491 / 19.4969 / 19.8199 kW**
   （点数 18,000 / 107,250 / 418,200），与其 `model_metadata.json` **逐位相同**
   ⇒ 共享管道与旧线的特征口径一致，A 可以放心复用 `ml/common/`。
   复现只需：`bundle = joblib.load(...)`，再 `bundle["models"][step].predict(x[bundle["feature_columns"]].values)`，
   然后按 `bundle["clip"]`（`min_kw = 0.0`，上限是字符串哨兵 `"station rated_capacity_kw"`，
   即逐行按该站额定功率裁剪）裁剪。
2. **旧产物的批次绑定是错的**：三份 `model_metadata.json` 的 `sourceManifestSha256` 等于
   `serving_manifest.json` 自身哈希（`358eeba3…`），契约要求的是原始包 manifest（`1f03e37c…`）。
   重训时必须改，否则"数据→查询库→API→模型同一批次可追溯"这条验收会挂。
   B 线已在 `ml/common/artifacts.py` 与 `data_io.py` 里把两者分开记录并加了断言测试。
3. 旧线配方（**从 pickle 里读出来的实际参数**，不是二手转述）：
   `HistGradientBoostingRegressor(learning_rate=0.06, max_iter=300, random_state=20260913)`，
   `early_stopping="auto"`、`validation_fraction=0.1`、`n_iter_no_change=10`，实测 `n_iter_ = 107 < 300`
   ⇒ 早停在生效，所以**重训时的行序与内部验证划分会改变结果**；本轮只复现了推理，没有复现训练，
   因此这条配方对 A 是起点而不是已验证结论。旧线输入 42 列，产物结构
   `{"models": {step: 回归器}, "feature_columns": [...], "horizon_hours": H, "clip": {...}}`。

## 附录 B：最小推理测试的实际输出

```
$ python -m data_analysis.ml.availability.predict --self-check --run-dir data_analysis/outputs/ml_avail_run6
[self-check] analytics_full_180d_v1 batch analytics-298aa3ee1401461fb06ea2bb96930dcf; 12 bundle(s); sample row 3
[SKIPPED] avail-hier-h01-coldDL: holdout trained without DL
... （9 个 coldstart bundle 同样 SKIPPED，原因写在行内）
[PASS] avail-hier-h01 ST-BJ-01@2026-04-28T19:00:00Z values=[3]
[PASS] avail-hier-h06 ST-BJ-01@2026-04-28T19:00:00Z values=[3, 3, 3, 3, 3, 2]
[PASS] avail-hier-h24 ST-BJ-01@2026-04-28T19:00:00Z values=[3, 3, 3, 3, 3, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 3, 3, 3, 3, 3, 3]
[self-check] 3 passed, 0 failed, 9 skipped
```

**本轮重绑前后的唯一差别是第一行的 run 目录与批次号**——三行 `PASS` 给出的整数桩序列一字未变
（旧记录是 `--run-dir .../ml_avail_run2` + `batch analytics-5f8e9342…`，其余行逐字符相同），
这是"重绑只动绑定、不动预测"最省事的复核口径。

每个 bundle 检查六件事：产物哈希与元数据一致、按当前发布批次可服务、点数 == 跨度、
时间戳是参考时刻之后的连续整点、值为 `0..capacity` 的整数、`risk()` 的分布概率和为 1 且
中位数落在自己的区间内。`--self-check` 退出码非 0 即表示有 bundle 不合格。

对应的单元测试 `MinimalInferenceTest` 还包含一条**变异检查**：把最小 bundle 复制进临时目录、
改掉 `model_metadata.json` 里的 `artifactSha256`，测试要求它必须变成 FAIL——
一个永远输出 PASS 的检查不算检查。
