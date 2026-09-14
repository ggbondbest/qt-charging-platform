# 充电站选址推荐线(gbdt-rank-incity-v1)

给司机在**本城 5 个站**里排一个个性化顺序:他这次最可能去哪个站。
数据事实支撑这个简化:6,000 用户的 121,539 次 STARTED 充电 100% 发生在 home city,
跨城不存在迁移,所以候选集=同城站,任务是组内排序而非全库召回。

与空闲桩预测的边界(第二个成员的任务):本线**不建模任何"未来可用性"**。
站点的弃单率/排队/同时段上电次数都是"截至事件时刻已发生"的历史统计,只当排序特征用。

## 数据与协议

- 只读 `datasets/analytics_full_180d_v1/clean/` 的 charging_attempts / charging_sessions /
  reviews / queue_entries / users / vehicles / stations / chargers;不写数据集目录,产物全在 `outputs/ml_recommend/`。
- 事件 = outcome 为 STARTED 的 attempt(121,539 个),标签=实际选中的站;
  ABANDONED(排队弃单 10,143、无桩可用 24,380)不进标签,只进站点滚动统计。
- 时间三段,互不重叠:**TRAIN < 2026-05-01 ≤ VALIDATION < 2026-05-15 ≤ TEST < 2026-05-30**。
  TRAIN 102,665 / VALIDATION 8,980 / TEST 9,894 个事件。
- 防泄漏纪律(全库统一口径):
  - 日级站点聚合先 `shift(1)` 再 28 天滚动——第 d 天的行只含 d 之前的历史;
  - 用户×站点对配历史用 `merge_asof(allow_exact_matches=False)`,携带"含该行"累计量,
    匹配到的最后一条即"严格早于 t 的全部";
  - 分时电价对同城 5 站无区分度,不进特征;`chosen_station_id`、`opened_at` 等列入 NON_FEATURE 并有 dtype 守卫。
- 36 个特征、5 组:站点静态与车桩匹配(功率 min/surplus、变压器、站龄)、
  站点 28d 滚动(评分、低分率、每 kWh 实付、弃单率×2、排队位次、started 率)、
  近 7d 同时段上电次数、用户全局与配对历史(次数/距今天数/最近访问标记/用户给该站的扩张均分)、
  时间与画像(小时、星期、周末、会员、细分、车级、注册时长)。

## 模型

`HistGradientBoostingClassifier`(pointwise 学 P(选中|特征),推理按分排序;类别列 from_dtype)。
400 棵树 / lr 0.05 / 31 叶 / min_leaf 40 / l2 1.0 / seed 42 —— **一次成型,未在验证集上调过第二轮超参**(避免挑选分支)。

## 结果(排序指标,1=猜中置顶;random 基线 hit@1≈0.2 与 1/5 相符,协议自洽)

VALIDATION(选型用):

| 方法 | hit@1 | hit@3 | MRR | NDCG@5 |
| --- | --- | --- | --- | --- |
| **模型** | **0.605** | **0.911** | **0.762** | **0.822** |
| 用户历史次数 | 0.548 | 0.819 | 0.707 | 0.780 |
| 忠诚度(最近去的站) | 0.441 | 0.810 | 0.645 | 0.733 |
| 人气(28d 单量) | 0.350 | 0.748 | 0.581 | 0.685 |
| 平均评分 | 0.200 | 0.651 | 0.463 | 0.595 |
| 随机 | 0.198 | 0.589 | 0.453 | 0.587 |

TEST 首盲(2026-05-15~29,只批阅一次,`write_new_json` 已冻结):

| 方法 | hit@1 | hit@3 | MRR | NDCG@5 |
| --- | --- | --- | --- | --- |
| **模型** | **0.593** | **0.913** | **0.756** | **0.818** |
| 用户历史次数 | 0.537 | 0.814 | 0.699 | 0.773 |
| 忠诚度 | 0.422 | 0.803 | 0.632 | 0.724 |
| 人气 | 0.323 | 0.735 | 0.560 | 0.669 |
| 平均评分 | 0.232 | 0.648 | 0.485 | 0.611 |
| 随机 | 0.201 | 0.595 | 0.456 | 0.589 |

正确站名次分布:1 位 59.3%、2 位 21.7%、3 位 10.3%、4 位 5.8%、5 位 2.9%。
TEST 与 VALIDATION 差 ≤1.2pt(hit@1),无分布断崖。
相对"老地方"忠诚度基线 +17.1pt hit@1、+0.12 MRR——模型不是只会复读用户惯性,
它同时吃进了站点质量信号(弃单率/评分/价位)与场景信号(时段×站型)。

## 限制

- 合成数据,成绩只能表述为"模拟数据测试结果"。
- 无用户实时位置/轨迹表,推荐不含真实距离(同城 5 站的可达性差异未建模)——如后续导出用户定位日志,可加 haversine 特征。
- 候选只有 5 站,指标天花板高、绝对值好看;跨城出行、新开站冷启动、非充电 POI 推荐不在本线范围。
- 在线契约(model contracts)目前只覆盖 load/availability;本线产物为离线 bundle,
  接入服务需 leader 定推荐响应 schema(建议 top-3 + 理由字段)。

## 复现

```bash
python -m data_analysis.ml.recommend.build_data   # 长表 607,695 行 + build_summary.json
python -m data_analysis.ml.recommend.train       # 拟合 + VALIDATION 基线对比表
python -m data_analysis.ml.recommend.evaluate    # TEST 首盲,重跑会拒绝覆盖
python -m unittest data_analysis.tests.test_ml_recommend
```

文件:`common.py`(装载/切分/排名指标/冻结写)、`build_data.py`(point-in-time 特征长表)、
`train.py`/`evaluate.py`、单测在 `data_analysis/tests/test_ml_recommend.py`。
