# 空闲桩数可用性预测 · 交付说明（机器学习成员 B）

读者：组长（用于接后端注册、评审与冻结验收）。
代码基线：`feature/phase2-data-layer @ 444b3fd`（已合入 `develop 067f7b9`）。
本地分支：`feature/phase2-ml-load`，**尚未推送、无上游**；分支名写的是 load，本分支实际内容是
availability（B 线），建议改名，见第 7 节。

> 全部指标为模拟数据测试结果（第二阶段发布批次），不代表真实运营数据表现。

## 0. 一句话结论

交付 1 / 6 / 24 小时三个跨度的**空闲充电桩数量**概率预测：每个未来小时一个有序分类
HistGradientBoosting 全分布 + 站点×小时层级经验先验，服务口径 TEST MAE
**0.4816 / 0.5047 / 0.5120 桩**，输出合法率 **1.0000**，`predict()` 直接通过
`contracts/model.py:validate_prediction`，**不改任何契约**。

## 1. 对照交接文档的交付清单

| 文档对 B 的要求 | 交付位置 | 状态 |
| --- | --- | --- |
| 建立 `ml/availability/` | `data_analysis/ml/availability/`（7 个模块 2,341 行；共享层 `ml/common/` 6 个模块 901 行；测试 `ml/tests/test_ml_contract.py` 723 行 / 36 个用例） | 完成 |
| 预测各未来小时**最后采样时刻**的空闲桩数 | `model.py` 标签 `label_available_count_hNN` = 应答小时最后采样时刻的空闲桩数；口径由 `train.py` 与 `test_the_label_is_the_free_count_of_the_hour_it_answers` 双向锁定 | 完成 |
| 先建基线，再训练树模型 | `evaluate.py` 六级基线阶梯（重复上小时 / 站点×小时中位数 / 站点×小时经验分布 / 场站类型×小时 / 城市×小时 / 全局中位数），只在 TRAIN 拟合 | 完成 |
| 树模型 | `train.py`（0.2.0 纯分类器）+ `build_hierarchical.py`（0.3.0 出厂先验版，复用 0.2.0 的估计器，只在 VALIDATION 上选 k、区间级别和点值规则） | 完成 |
| 评估 | `evaluate.py` → `evaluation_report.md/.json`（分跨度、分城市/站点、冷启动留出、区间覆盖、风险阈值） | 完成 |
| 评价报告 | **`outputs/ml_avail_eval_run2_v2/evaluation_report.md`**（本版为准：多出第 6 节分城市表与报告头的复现命令行，其余每个分数与 `ml_avail_eval_run2` 逐字符相同，旧目录一字未动）。因 `outputs/` 被 gitignore，关键数字全部抄进本文件第 5 节 | 完成 |
| 推理 | `predict.py::AvailabilityForecaster.predict()`（契约点预测）与 `.risk()`（小数预计值、区间、P(无桩)，独立侧信道） | 完成 |
| 报告 MAE 与输出范围合法性 | 第 5 节表格；合法率 1.0000，且是结构性结果（模型只能输出 0..capacity 整数，推理再裁剪一次） | 完成 |
| 不能把预计小数桩数当真实库存 | 对外 `points[].value` 恒为 Python `int`；期望值只在 `risk()` 的 `expectedChargers` 里，字段名与 `definition` 文案都标注为预计值；由 `MinimalInferenceTest` 机械检查 | 完成 |
| 模型文件及预处理器 | `data_analysis/outputs/ml_avail_run2/{h01,h06,h24}/model.joblib`（bundle 内含特征列顺序、站点/日历/城市档案、先验表 → 预处理与模型同包，不需要第二个文件） | 完成 |
| `model_metadata.schema.json` 要求的元数据 | 每 bundle 的 `model_metadata.json`，由 `artifacts.build_metadata()` 生成并 `validate_metadata()` 校验；`metrics` 严格只含 `mae/rmse/testSamples/unit` | 完成 |
| 可复现训练命令 | 本文件第 3 节，且评估报告头部现在自带复现命令行；代码侧 `artifacts.invocation()` 把命令与种子写入此后每次训练产出的 `training_report.json`（run1/run2 先于该字段，见 3.1） | 完成 |
| 最小推理测试 | `python -m data_analysis.ml.availability.predict --self-check --run-dir ...`，并同步为 `ml/tests` 的 `MinimalInferenceTest`（4 个用例）；另有 `RunDirectoryGuardTest`（4 个，锁"不覆盖已发布 run"）与 `InvocationRecordTest`（3 个，锁复现命令本身可执行） | 完成 |
| 独立扩展（择一） | **未做**。按「时间不足时先交付基础预测闭环」，先把基础线补完；扩展建议与数据前提见第 6 节 | 待组长定 |

## 2. 输入批次与绑定（全部实测，非文档转抄）

| 项 | 值 |
| --- | --- |
| datasetId | `charging_full_180d_v2` |
| pipelineRunId | `spark-625439908b6d4bcca1ae72e2e35ef27f` |
| publishedBatchId | `analytics-5f8e93429948404099b735999bfb6c4e` |
| 原始数据清单 sha256（`sourceManifestSha256`） | `1f03e37c644ec928f8de0511b614b316bcc35632056891173d2377b2733ccb95` |
| 导出清单 sha256（仅记录，不写进 `sourceManifestSha256`） | `358eeba3d3ceac9fab7858a6293a02575b11150f43454ce575f4408c687eaa59` |
| featureVersion | `history24-v1`，模型输入 75 列（不含任何 `label_` / `split_` 列，由测试锁定） |
| 训练帧 | 107,425 行 × 75 特征；25 站、5 城（BJ/DL/SH/SY/SZ）、5 种场站类型；每站 `capacity = 3` 桩，故标签取值 {0,1,2,3} |
| 切分（`mlSplits`，上海业务日右开） | TRAIN 2025-12-02→2026-03-30，VALIDATION →2026-04-29，TEST →2026-05-29 |
| 各跨度行数 | 1h 70,800/18,000/18,000/EXCLUDED 625；6h 70,675/17,875/17,875/1,000；24h 70,225/17,425/17,425/2,350 |

**批次绑定纪律**：`sourceManifestSha256` 绑的是**原始包 manifest**（`1f03e37c…`）。导出包
`serving_manifest.json` 自身的哈希（`358eeba3…`）另记在 `training_report.json` 的
`servingManifestSha256`。测试
`test_bundle_is_bound_to_the_batch_it_reads` 明确断言二者不相等——负荷线 0.1.0 产物正是把后者
误写进了前者（见附录 A）。

## 3. 启动方法（仓库根目录执行，需 numpy/pandas/scikit-learn/joblib）

```bash
# 0) 数据画像（可选，只读导出包并逐分片校验 manifest）
python -m data_analysis.ml.availability.prepare_data --output data_analysis/outputs/ml_avail_run1

# 1) 树模型：纯有序分类器 + 三个城市的冷启动留出（约 70 分钟，seed 20260913）
python -m data_analysis.ml.availability.train \
    --output data_analysis/outputs/ml_avail_run1 \
    --holdout-city DL --holdout-city SY --holdout-city SZ

# 2) 出厂先验版：复用第 1 步的估计器，只在 VALIDATION 上选每层 k / 区间级别 / 点值规则（约 88 秒）
python -m data_analysis.ml.availability.build_hierarchical \
    --source-run data_analysis/outputs/ml_avail_run1 \
    --output data_analysis/outputs/ml_avail_run2

# 3) 评估（写新目录，绝不覆盖已发布报告；报告头会自动记下这条命令）
python -m data_analysis.ml.availability.evaluate \
    --run-dir data_analysis/outputs/ml_avail_run2 \
    --output data_analysis/outputs/ml_avail_eval_run2_v2

# 4) 最小推理测试：12 个 bundle 逐个加载、按契约服务一个确定 TEST 行，任一非法即退出码 1
python -m data_analysis.ml.availability.predict --self-check --run-dir data_analysis/outputs/ml_avail_run2

# 5) 单次推理（后端适配器的调用形态）
python -m data_analysis.ml.availability.predict \
    --bundle data_analysis/outputs/ml_avail_run2/h06 \
    --station ST-BJ-01 --reference-time 2026-04-28T19:00:00Z --horizon 6 \
    --export data_analysis/datasets/analytics_full_180d_v1
```

三个**会写产物**的入口（`train` / `build_hierarchical` / `evaluate`）都在装载数据帧之前拒绝覆盖：
目标目录已有 bundle、已有 `train_summary.json` 或已有 `evaluation_report.*` 时直接退出，要求写新目录；
`predict` 不写文件，改由 `--self-check` 的退出码把关。这三条拒绝各有单元测试（`RunDirectoryGuardTest`
与 `AuditScriptTest`），所以"不覆盖已发布产物"是可核查的约定，不是口头承诺。

### 3.1 可复现性的现状与边界（如实说明）

- 随机种子：`--seed 20260913`（train/build_hierarchical 默认值）。run 级文件
  （`train_summary.json`、`hierarchical_report.json`）记录了 seed 与两个 manifest 哈希；
  **依赖版本**记在每个 bundle 的 `model_metadata.json` 的 `dependencies` 字段里
  （`artifacts.dependency_versions()`：python/numpy/pandas/scikit-learn/joblib/platform），
  因为那才是加载产物真正需要的东西。
- 本次新增 `artifacts.invocation()`：此后每一次训练产出的**每个 bundle** 的 `training_report.json`
  都会带 `reproducibleCommand`（含全部命令行参数）与 `seed`，`evaluate.py` 的报告头同样自带复现命令行
  （`ml_avail_eval_run2_v2` 已是这样产出的）。`-m` 运行的模块名从 run spec 还原，不会写成 `__main__`
  ——这条由 `InvocationRecordTest` 锁住，因为一份写着 `-m __main__` 的报告等于没有复现命令。
- **已发布的 run1 / run2 不含该字段**（它们先于这次改动）。本轮逐个读过 `ml_avail_run1`、`ml_avail_run2`
  全部 15 份 `training_report.json` 与两份 run 级文件：`reproducibleCommand` **全部为空**，
  run2 的 bundle 连 `seed` 也没写（run 级 `hierarchical_report.json` 有 seed）。
  补齐的代价分两层，别混为一谈（这是上一版本报告写错的地方）：
  * **服务产物（run2 的 12 个 bundle）重跑只要 90 秒**——它们由 `build_hierarchical` 产出，
    当时日志记录的实测耗时是 **88.3 s**。但重跑写进 bundle 的是**构建命令**，
    仍然不含训练命令，因为 `--source-run` 指向的 run1 没记下自己的命令。
  * **要让产物真的带着训练命令，必须重跑 `train`**（run1 目录时间戳跨度 21:34:32→22:43:27，
    约 **69 分钟**，含 3 个主模型 + 9 个冷启动），随后再 build + evaluate。
  上面第 3 节的命令按各 run 报告里记录的参数与模块默认值重建，等价但不逐字节证明当时输入。
  是否值得为这个字段重跑，请组长拍板；**当前状态下的可复现路径是"照第 3 节敲命令"，不是"打开产物看命令"。**
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

### 5.1 出厂模型 `ml_avail_run2`（0.3.0，服务口径，TEST）

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
`ml_avail_eval_run2_v2/evaluation_report.md` 第 8 节；`ml_avail_eval_run1_corrected` 是用修正后的
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

### 5.6 分城市与最差站点（主模型，服务口径，TEST；新增于 `ml_avail_eval_run2_v2` 第 6 节）

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
说明误差不是均匀分布的。为什么是 04 号站**未经证实**（推测是这批站的空闲数在 0..3 全区间摆动更多），
接入侧若要按站点做告警阈值，应先单独看这几站的曲线，不要把城市级 MAE 当站点级用。

### 5.7 测试执行记录（`outputs/` 不进 git，故把跑测结果也抄在这里）

```
$ python -m unittest discover -s data_analysis/ml/tests -t . -v
Ran 36 tests in 768.718s
OK          # 退出码 0
```

环境：Python 3.13.9 / scikit-learn 1.7.2 / pandas 2.3.3 / numpy 2.4.4（与 `model_metadata.json`
的 `dependencies` 同一台机器同一环境）。**耗时 769 s 不是 flaky，是真的在 107,425 行的发布批次上
建特征、拟合小样本并加载 12 个 bundle**；因此 CI 目前不跑它（第 7 节第 2 条）等于这一层没有守护。

36 个用例按主题分布（`ml/tests/test_ml_contract.py`）：

| 测试类 | 个数 | 锁住的是什么（按用例名如实列，不夸大） |
| --- | --- | --- |
| `ShippedBundleTest` | 6 | bundle 与其读取的批次互相绑定；对外结果过 `validate_prediction`；层级 bundle 用的是自己那份先验；站点档案保持在线构建器能读的数值块；在线行能被档案完全填满；请求不能把答案当历史偷渡进来 |
| `PriorShrinkageTest` | 8 | 查表单元按"它描述的小时"而非"它被写下的时刻"取键；支持度只数 TRAIN 的小时；历史越多越偏向查表（run1 老报告把这个方向写反了）；每级用自己的伪计数；没见过的行退回模型；借来的单元按缺失的那个站池化；并列分布上中数优于众数；混合结果仍是合法 pmf 且能取到端点 |
| `FeatureTransformTest` | 5 | 在线特征与离线导出逐列一致；滞后窗口止于第一个应答小时前一刻；历史不完整/非法即拒；标签=应答小时最后采样时刻的空闲桩数；标签列与划分列不得出现在输入里 |
| `MinimalInferenceTest` | 4 | `--self-check` 下每个已发布 bundle 要么 PASS 要么被显式 SKIPPED；对外值是 `0..capacity` 的整数（小数只走 `risk()`）；改掉 `artifactSha256` 必须变 FAIL；命令本身退出码为 0 |
| `RunDirectoryGuardTest` | 4 | `train` 拒绝已放 bundle 的目录；拒绝"写了一半"的 run；`build_hierarchical` 同样拒绝有内容的目录；`prepare_data` 先写的 `data_profile.json` 不算已发布（否则正常流程会被自己的守卫挡住） |
| `DataBindingTest` | 3 | 导出必须绑到原始包 manifest（成员 A 旧产物正是把 serving manifest 当成了它）；无法核验的导出直接拒绝；`ml_targets_hourly` 永不进服务库 |
| `AuditScriptTest` | 3 | 审计脚本自己的收缩方向与取键口径；已发布报告不被就地改写 |
| `InvocationRecordTest` | 3 | `-m` 运行记录模块名而不是 `__main__`；直接按文件路径运行时记录路径；命令行参数逐字记录 |

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

1. **分支名**：`feature/phase2-ml-load` 未推送，内容与名字不符（实为 B 的可用性线）。
   建议 `git branch -m feature/phase2-ml-availability`；若 A 也要用同名分支请改 A 的。
   另外 `data_analysis/ml/PLAN.md`（未跟踪）是一份以**成员 A 负荷线**为主角的调研稿，与 B 的交付无关，
   请定：删掉、移交成员 A，还是标注为"A 线调研（由 B 的会话代查）"。
2. **CI 不跑 ml 测试，也没有任何地方声明 ML 依赖**：`.github/workflows/data-analysis.yml` 只有
   `generator` / `api` / `spark` 三个 job（已逐个看过），**没有一个跑 `data_analysis/ml/tests`**；
   仓库现有的三个清单（`data_analysis/requirements-api.txt`、`-spark.txt`、`-contract-test.txt`）
   **都不含 numpy / pandas / scikit-learn / joblib**（已逐个 grep 确认）。也就是说本线代码的运行环境
   目前只写在每个产物的 `model_metadata.json.dependencies` 里，从仓库里**装不出来**，
   换一台干净机器照第 3 节敲命令会在 `import pandas` 就断。
   这 36 个用例是本次全部"机械核查"的落点，建议加一个装科学计算包、跑
   `python -m unittest discover -s data_analysis/ml/tests -t .` 的 job，并新增一份
   `data_analysis/requirements-ml.txt`（**此文件尚不存在**）。CI workflow 与 requirements 都是共享文件，
   我不改，只提案；建议的最小补丁我可以随 PR 附上。
3. **模型注册入口缺失**：`backend` 目前无 `MODEL_REGISTRY` / `ANALYTICS_MODELS_DIR`，
   `tests/test_analytics_api.py` 断言两个 `/predict/*` 必须 503。我按文档要求交付推理代码
   （`AvailabilityForecaster` + 错误码），接入 `service.py` 的动作留给组长；需要组长确认
   注册目录约定与"加载失败仍回 503"的行为。
4. **是否重跑一遍模型产物**：评估报告已经自带复现命令（`ml_avail_eval_run2_v2`），但**模型 bundle 的
   `training_report.json` 仍缺 `reproducibleCommand`**（本轮逐个读过 15 份报告确认，详见 3.1），
   因为 run1/run2 先于这次改动。分两种花法：
   * 只花 **90 秒**重跑 `build_hierarchical`（新目录、分数应逐位相同）→ bundle 带上**构建命令**，
     但训练命令仍进不了产物；
   * 花 **约 69 分钟**重跑 `train` + build + evaluate（新目录 `ml_avail_run3`）→ 每个 bundle 才真正
     自带训练命令与 seed。`HistGradientBoosting` 只在同版本、同 seed 下确定性，所以"分数逐位相同"
     是预期而非保证，重跑后要重发评估报告。
   我倾向：**先不重跑**，等接口/注册口径定了再一次性出 `run3`，免得同一批产物出现第三个版本号；
   但这条由组长定。

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
$ python -m data_analysis.ml.availability.predict --self-check --run-dir data_analysis/outputs/ml_avail_run2
[self-check] analytics_full_180d_v1 batch analytics-5f8e93429948404099b735999bfb6c4e; 12 bundle(s); sample row 3
[SKIPPED] avail-hier-h01-coldDL: holdout trained without DL
... （9 个 coldstart bundle 同样 SKIPPED，原因写在行内）
[PASS] avail-hier-h01 ST-BJ-01@2026-04-28T19:00:00Z values=[3]
[PASS] avail-hier-h06 ST-BJ-01@2026-04-28T19:00:00Z values=[3, 3, 3, 3, 3, 2]
[PASS] avail-hier-h24 ST-BJ-01@2026-04-28T19:00:00Z values=[3, 3, 3, 3, 3, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 3, 3, 3, 3, 3, 3]
[self-check] 3 passed, 0 failed, 9 skipped
```

每个 bundle 检查六件事：产物哈希与元数据一致、按当前发布批次可服务、点数 == 跨度、
时间戳是参考时刻之后的连续整点、值为 `0..capacity` 的整数、`risk()` 的分布概率和为 1 且
中位数落在自己的区间内。`--self-check` 退出码非 0 即表示有 bundle 不合格。

对应的单元测试 `MinimalInferenceTest` 还包含一条**变异检查**：把最小 bundle 复制进临时目录、
改掉 `model_metadata.json` 里的 `artifactSha256`，测试要求它必须变成 FAIL——
一个永远输出 PASS 的检查不算检查。
