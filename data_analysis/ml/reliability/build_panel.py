"""生成第七线的新增派生数据集：``data_analysis/derived/charger_day_panel_v1/``（独立目录，不写发布批次）。

为什么必须有这份新数据：clean 层只有事件表，**没有任何日粒度表**。第七线的样本单位是"一台桩的一个
北京日历日"，特征要的是"这台桩昨天为止每天被用几次、失败几次"、"这个城市昨天热不热、下没下雨"——
这些量在原始数据里不存在，不聚合出来就没有可训练的东西。所以本文件按用户授权新增数据。

新增的口径（写进 ``derivation_manifest.json``，也写进本目录 README）：
  · ``panel_charger_day.csv``——每台桩 × 每个北京日历日的**稠密**面板（13,500 行 = 75 台 × 180 天，
    含当日 0 次尝试的日子，显式记 0，不靠"缺行"表达"没发生"）；
  · ``day_context.csv``——每城 × 每日的日历事实与天气日聚合（900 行 = 5 城 × 180 天）。

派生方式全部是**对发布批次真实记录的确定性聚合**（groupby + sum/mean/max/mode），没有编造的标签、
没有新增事件、没有改任何原始行。因此本线不能沿用"只读既有 clean 表"的说法，报告里必须分开写清：
指标口径是模拟数据，特征表是发布批次的派生聚合。

与第六线的对账（结构断言，换批次后前提变了会直接构建失败）：面板总尝试数必须恰好等于第六线样本域
102,801，技术失败总数必须恰好等于 3,676——两条线用的是同一批事实，只是切成不同的样本单位。

用法（仓库根目录）：python -m data_analysis.ml.reliability.build_panel
"""

from __future__ import annotations

from pathlib import Path

import pandas as pd

from . import common

#: 面板列。计数列一律 int64，避免下游 pandas 在 0 与小数量之间猜成 float 后又回头改 dtype。
PANEL_COLUMNS = ["charger_id", "station_id", "city_id", "business_date", "attempts_on_day",
                "started_on_day", "tech_fails_on_day", "fail_flag", "users_on_day"]
CONTEXT_KEY = ["city_id", "business_date"]

#: 本线不读的表与不读的列，逐条留原因——"读没读过"在换批次时一定有人问。
IGNORED_SOURCES = {
    "anomaly_labels": "第四线的事后人工标签，对'明天会不会启动失败'是答案侧信息，本线整表不读",
    "maintenance_tickets.status": "全表终态，'现在已解决'不等于'当时已解决'；工单只用 reported_at/restored_at",
    "maintenance_tickets.accepted_at/work_started_at": "调度侧时间戳，真实系统里夜间可否读到未定，保守不用",
    "maintenance_tickets.labor_cost_cents/parts_cost_cents": "钱是修完才知道的量，对次日风险无前置信息",
    "tariffs": "电价在 (城, 小时) 上是常数，日级聚合后成为**城市级常量**——等价于把 ban 掉的 city_id "
              "换个马甲带进特征（第六线同样以'纯身份列'禁掉 city_id），本线不读",
    "charging_sessions": "会话级电量/时长是'这趟充得怎么样'（第一/四线），本线的暴露度已由 "
                        "attempts_on_day 表达，且会话表里没有次日可得的前置信息",
    "payments/reviews/operating_costs/corruption_log/battery_samples/vehicle_energy_intervals/"
    "queue_entries/reservations/campaigns": "与'次日这台桩会不会启动失败'无因果关系，或需事后才可得",
}


# --------------------------------------------------------------------------- 事件装载（与第六线同口径）


def load_in_scope_attempts() -> tuple[pd.DataFrame, dict]:
    """读 charging_attempts，筛出与第六线完全一致的样本域，再打北京日历日。

    口径必须与第六线逐字相同（非排队关联 ∧ charger_id 非空 ∧ outcome ∈ {STARTED, FAILED}），
    否则两条线的对账断言就是假的。这里刻意重算而不是 import 第六线的函数：第七线是独立一条线，
    共用了别人的构建函数，第六线一改本线就会"悄悄跟着变"。
    """
    table = common.load_clean_table("charging_attempts")
    assert table["attempt_id"].is_unique, "attempt_id 不唯一，一行一次尝试的前提不成立"
    assert table["attempted_at"].notna().all()
    table = table.copy()
    table["attempted_at"] = pd.to_datetime(table["attempted_at"])
    reason = table["failure_reason"]
    tech = reason.isin(common.TECHNICAL_FAILURE_REASONS)
    # 与第五线切割的事实（本线继承第六线的前提）：带排队的尝试里技术失败为 0
    linked = table["queue_id"].notna()
    assert int((tech & linked).sum()) == 0, "带排队的尝试出现技术失败，第五/六线切割前提变了"
    assert tech.eq(table["outcome"] == "FAILED").all(), "三类技术原因与 FAILED 不再一一对应"
    keep = (table["queue_id"].isna() & table["charger_id"].notna()
            & table["outcome"].isin(["STARTED", "FAILED"]))
    sample = table[keep].copy()
    sample["attempted_local"] = sample["attempted_at"] + pd.Timedelta(
        hours=common.BUSINESS_OFFSET_HOURS)
    sample["business_date"] = sample["attempted_local"].dt.normalize()
    sample["is_tech_fail"] = sample["failure_reason"].isin(common.TECHNICAL_FAILURE_REASONS).astype("int64")
    # STARTED 的充要条件是 session_id 非空（第六线实测核对过的数据事实），这里预先转成整数列，
    # 免得在 groupby 里挂 lambda——慢，而且聚合口径不可读。
    sample["started_flag"] = sample["session_id"].notna().astype("int64")
    notes = {
        "totalAttempts": int(len(table)),
        "inScopeRows": int(len(sample)),
        "techFailures": int(sample["is_tech_fail"].sum()),
        "businessDateRange": [str(sample["business_date"].min().date()),
                             str(sample["business_date"].max().date())],
        "scopeRule": "非排队关联 ∧ charger_id 非空 ∧ outcome ∈ {STARTED, FAILED}（与第六线逐字相同）",
    }
    return sample, notes


# --------------------------------------------------------------------------- 桩×日面板


def build_charger_day(sample: pd.DataFrame) -> tuple[pd.DataFrame, dict]:
    """稠密的"每台桩 × 每个北京日历日"面板：包括当日 0 次尝试的日子。

    为什么要稠密：本线的特征有"日均尝试量""连续多少天没失败"这类按**日历日**定义的聚合，
    若只存有尝试的日子，"日均"就会变成"每个被使用日的均值"，同一个数值在两台桩上含义不同
    （一台天天用、一台偶尔用），而这正是本线最怕的暴露度混杂。
    """
    chargers = common.load_clean_table("chargers", columns=["charger_id", "station_id"])
    stations = common.load_clean_table("stations", columns=["station_id", "city_id"])
    assert chargers["charger_id"].is_unique and stations["station_id"].is_unique
    assert set(sample["charger_id"].unique()) <= set(chargers["charger_id"]), \
        "出现 chargers 表里没有的桩，静态属性无从取得"
    base = sample.groupby(["charger_id", "business_date"], observed=True).agg(
        attempts_on_day=("attempt_id", "size"),
        started_on_day=("started_flag", "sum"),
        tech_fails_on_day=("is_tech_fail", "sum"),
        users_on_day=("user_id", "nunique"),
    ).reset_index()

    first = pd.Timestamp(min(sample["business_date"]))
    last = pd.Timestamp(max(sample["business_date"]))
    days = pd.date_range(first, last, freq="D", name="business_date")
    grid = pd.MultiIndex.from_product([chargers["charger_id"].to_numpy(), days],
                                      names=["charger_id", "business_date"]).to_frame(index=False)
    panel = grid.merge(base, on=["charger_id", "business_date"], how="left", validate="one_to_one")
    counts = ["attempts_on_day", "started_on_day", "tech_fails_on_day", "users_on_day"]
    missing_days = int(panel["attempts_on_day"].isna().sum())
    for column in counts:
        panel[column] = panel[column].fillna(0).astype("int64")
    assert (panel["attempts_on_day"] >= 0).all()
    assert (panel["tech_fails_on_day"] <= panel["attempts_on_day"]).all(), \
        "某桩某日技术失败数大于尝试数，标签口径出错"
    assert (panel["started_on_day"] <= panel["attempts_on_day"]).all(), "STARTED 数不可能超过尝试数"
    panel["fail_flag"] = (panel["tech_fails_on_day"] > 0).astype("int64")
    panel = (panel.merge(chargers, on="charger_id", how="left", validate="many_to_one")
                  .merge(stations, on="station_id", how="left", validate="many_to_one"))
    assert panel[["station_id", "city_id"]].notna().all().all(), "面板存在取不到站/城的桩"
    panel = panel[PANEL_COLUMNS].sort_values(["business_date", "charger_id"],
                                            kind="stable").reset_index(drop=True)
    notes = {
        "rows": int(len(panel)),
        "chargers": int(panel["charger_id"].nunique()),
        "days": int(panel["business_date"].nunique()),
        "zeroAttemptDays": missing_days,
        "attemptsTotal": int(panel["attempts_on_day"].sum()),
        "techFailsTotal": int(panel["tech_fails_on_day"].sum()),
        "reconcilesWithLine6": bool(panel["attempts_on_day"].sum() == sample.shape[0]
                                    and panel["tech_fails_on_day"].sum() == int(sample["is_tech_fail"].sum())),
        "sampleRowsWithAttempt": int((panel["attempts_on_day"] > 0).sum()),
        "failDayRateAmongUsed": round(float(panel.loc[panel["attempts_on_day"] > 0, "fail_flag"].mean()), 5),
        "failDayRateAmongZero": round(float(panel.loc[panel["attempts_on_day"] == 0, "fail_flag"].mean()), 5),
        "exposureGradient": {str(label): round(float(rate), 5) for label, rate in
                             panel.loc[panel["attempts_on_day"] > 0]
                             .assign(bucket=pd.cut(panel.loc[panel["attempts_on_day"] > 0, "attempts_on_day"],
                                                  [0, 2, 5, 10, 20, 10 ** 9]))
                             .groupby("bucket", observed=True)["fail_flag"].mean().items()},
    }
    # 0 次尝试的日子必然 0 失败——这就是本线绝不能把当日尝试数当特征的理由，也是训练样本
    # 只取"当日 ≥1 次尝试"的理由（否则模型靠"今天没人用"白拿一半 AUC）。
    assert notes["failDayRateAmongZero"] == 0.0, "0 次尝试的日子出现了失败，样本口径出错"
    assert notes["reconcilesWithLine6"], "面板与第六线样本域对不上账"
    return panel, notes


# --------------------------------------------------------------------------- 城市×日上下文


def build_day_context() -> tuple[pd.DataFrame, dict]:
    """每城 × 每日：日历事实 + 天气日聚合（气温均值/极值、湿度、雨量、降雨小时数、主导天气）。

    天气按**北京日历日**聚合（recorded_at + 8h 取日），因为面板的日子也是北京日历日；聚合本身
    不含因果判断——"哪天能读到"是特征层的事（本线只用**昨天及更早**的天气日聚合，见 features.py），
    本文件存的就是当天的事实。
    """
    calendar = common.load_clean_table("calendar")
    calendar["business_date"] = pd.to_datetime(calendar["business_date"])
    assert calendar[CONTEXT_KEY].drop_duplicates().shape[0] == len(calendar), "日历表同城同日多行"
    weather = common.load_clean_table("weather_hourly", columns=["city_id", "recorded_at",
                                                                 "temperature_c", "humidity_pct",
                                                                 "weather", "rainfall_mm"])
    weather["recorded_at"] = pd.to_datetime(weather["recorded_at"])
    weather["business_date"] = (weather["recorded_at"] + pd.Timedelta(
        hours=common.BUSINESS_OFFSET_HOURS)).dt.normalize()
    assert weather["temperature_c"].notna().all(), "天气表存在缺失气温的小时"
    # 电价表这里**只核对可得性，不产出特征列**：城-小时上的常数价按日聚合后就是城市级常量，
    # 等于把禁入的 city_id 换个马甲带进来（见 IGNORED_SOURCES["tariffs"]）。
    tariffs = common.load_clean_table("tariffs", columns=["city_id", "hour", "period"])
    assert not tariffs.duplicated(subset=["city_id", "hour"]).any(), "电价表在 (城, 小时) 上不唯一"
    grouped = weather.groupby(CONTEXT_KEY, observed=True).agg(
        temp_c_mean=("temperature_c", "mean"),
        temp_c_min=("temperature_c", "min"),
        temp_c_max=("temperature_c", "max"),
        humidity_mean=("humidity_pct", "mean"),
        rainfall_mm_sum=("rainfall_mm", "sum"),
        rain_hours=("rainfall_mm", lambda s: int((s > 0).sum())),
        weather_hours=("weather", "size"),
        weather_mode=("weather", lambda s: str(s.mode().iloc[0])),
    ).reset_index()
    context = calendar.merge(grouped, on=CONTEXT_KEY, how="left", validate="one_to_one")
    assert context["weather_mode"].notna().all(), "存在没有天气聚合的城-日"
    context["is_weekend"] = context["is_weekend"].astype("int64")
    context["rain_hours"] = context["rain_hours"].astype("int64")
    for column in ("temp_c_mean", "temp_c_min", "temp_c_max", "humidity_mean", "rainfall_mm_sum"):
        context[column] = context[column].round(4)
    # 列序固定下来：面板与上下文都是入仓文件，列序变了就等于数据变了（verify_derived 会抓到，
    # 但把列序写死比事后抓到更好）。
    context = (context.rename(columns={"weather_mode": "weather"})
               [["city_id", "business_date", "is_weekend", "scenario_event", "demand_multiplier",
                 "temp_c_mean", "temp_c_min", "temp_c_max", "humidity_mean", "rainfall_mm_sum",
                 "rain_hours", "weather_hours", "weather"]]
               .sort_values(CONTEXT_KEY, kind="stable").reset_index(drop=True))
    notes = {
        "rows": int(len(context)),
        "cities": int(context["city_id"].nunique()),
        "days": int(context["business_date"].nunique()),
        "weatherHoursPerCityDay": {"p50": float(context["weather_hours"].median()),
                                  "min": int(context["weather_hours"].min()),
                                  "max": int(context["weather_hours"].max())},
        "temperatureRangeC": [round(float(context["temp_c_min"].min()), 2),
                             round(float(context["temp_c_max"].max()), 2)],
        "rainDaysShare": round(float((context["rainfall_mm_sum"] > 0).mean()), 4),
        "scenarioEvents": {str(k): int(v) for k, v in context["scenario_event"].value_counts().items()},
        "weatherStates": {str(k): int(v) for k, v in context["weather"].value_counts().items()},
    }
    return context, notes


# --------------------------------------------------------------------------- 落盘


def main() -> dict:
    common.require_empty_run_dir(common.DERIVED_DIR)
    source_manifest = common.verify_batch()
    sample, attempt_notes = load_in_scope_attempts()
    panel, panel_notes = build_charger_day(sample)
    context, context_notes = build_day_context()

    # 面板里的每个城-日都必须能在上下文里找到（同城同日缺一行，特征层就会造出一整列 NaN）。
    # 用 merge 而不是 set 成员判断：Timestamp 与 np.datetime64 的哈希不相等，集合判断会假报缺失。
    need = panel[["city_id", "business_date"]].drop_duplicates().assign(_present=1)
    coverage = need.merge(context[CONTEXT_KEY].assign(_found=1), on=CONTEXT_KEY, how="left")
    gaps = coverage[coverage["_found"].isna()]
    assert gaps.empty, (f"day_context 缺少 {len(gaps)} 个城-日，"
                        f"例如 {gaps[CONTEXT_KEY].head(3).to_dict('records')}")

    generator_sha = common.sha256_file(Path(__file__))

    files = []
    for name, frame in ((common.PANEL_NAME, panel), (common.CONTEXT_NAME, context)):
        path = common.DERIVED_DIR / f"{name}.csv"
        common.write_new_csv(path, frame)
        files.append({"name": f"{name}.csv", "rows": int(len(frame)),
                      "columns": list(frame.columns), "sha256": common.sha256_file(path)})

    summary = {
        "derivedDatasetId": common.DERIVED_DATASET_ID,
        "kind": "derived（对发布批次真实记录的确定性聚合，非新增事件、非编造标签）",
        "sourceBatchId": common.EXPECTED_PUBLISHED_BATCH_ID,
        "sourcePipelineRunId": source_manifest.get("pipelineRunId"),
        "sourceDatasetId": common.DATASET_ID,
        "generator": {"module": "data_analysis.ml.reliability.build_panel",
                      "sha256": generator_sha},
        "sourceTables": {"charging_attempts": attempt_notes["totalAttempts"],
                        "chargers": int(len(common.load_clean_table("chargers", ["charger_id"]))),
                        "stations": int(len(common.load_clean_table("stations", ["station_id"]))),
                        "calendar": int(len(common.load_clean_table("calendar"))),
                        "weather_hourly": int(len(common.load_clean_table(
                            "weather_hourly", columns=["city_id"]))),
                        "tariffs": int(len(common.load_clean_table("tariffs", ["city_id"])))},
        "labelDefinition": "tech_fails_on_day = failure_reason ∈ CONNECTOR_HANDSHAKE/APP_TIMEOUT/"
                          "AUTH_FAILED 的当日次数；口径与第六线一致",
        "attemptScope": attempt_notes,
        "panel": panel_notes,
        "dayContext": context_notes,
        "notRead": IGNORED_SOURCES,
        "causalityNote": "本文件只做聚合，不做因果判断：天气/遥测的'哪天能读到'在 features.py 里"
                        "统一按'严格早于当日起点'处理，day_context 存的是当天事实",
        "files": files,
        "disclaimer": common.data_note(),
    }
    common.write_new_json(common.DERIVED_MANIFEST_PATH, summary)
    common.write_new_text(common.DERIVED_DIR / "README.md", render_readme(summary))

    print(f"[reliability] 新增派生数据集 {common.DERIVED_DIR}")
    print(f"[reliability] 面板 {len(panel):,} 行 = {panel_notes['chargers']} 台桩 × "
          f"{panel_notes['days']} 天；其中当日 ≥1 次尝试 {panel_notes['sampleRowsWithAttempt']:,} 行")
    print(f"[reliability] 对账：尝试 {panel_notes['attemptsTotal']:,} / 技术失败 "
          f"{panel_notes['techFailsTotal']:,}（与第六线一致={panel_notes['reconcilesWithLine6']}）")
    print(f"[reliability] 上下文 {len(context):,} 行 = {context_notes['cities']} 城 × "
          f"{context_notes['days']} 天")
    print(f"[reliability] 暴露度梯度（当日失败日占比）：{panel_notes['exposureGradient']}")
    return summary


def render_readme(summary: dict) -> str:
    """目录内 README：让"这份数据从哪来、怎么再生成、能不能改"在数据旁边就能读到。"""
    panel = summary["panel"]
    gradient = "\n".join(f"| {k} | {v:.4f} |" for k, v in panel["exposureGradient"].items())
    lines = "\n".join(f"- `{k}`：{v}" for k, v in summary["notRead"].items())
    files = "\n".join(f"- `{f['name']}`：{f['rows']:,} 行，sha256 `{f['sha256'][:16]}…`"
                      for f in summary["files"])
    return f"""# {summary['derivedDatasetId']}（第七线新增派生数据集）

本目录是**派生数据**，不是第二阶段发布批次的一部分：全部列都是对发布批次
`{summary['sourceBatchId']}`（run `{summary['sourcePipelineRunId']}`）真实记录的确定性聚合
（groupby + sum/mean/max/mode），**没有编造标签、没有新增事件、没有修改任何原始行**。
`datasets/analytics_full_180d_v1/` 一个字节都没动。

重新生成（会拒绝覆盖已有目录）：

```
python -m data_analysis.ml.reliability.build_panel
```

## 文件

{files}

`derivation_manifest.json` 记录源批次、源表行数、生成器代码哈希与全部口径；
`data_analysis/ml/reliability/common.py:verify_derived()` 在每次读取时复查行数与 sha256，
对不上就拒用。

## panel_charger_day.csv

每台桩 × 每个北京日历日的稠密面板（{panel['chargers']} 台 × {panel['days']} 天 = {panel['rows']:,} 行）。
当日 0 次尝试的日子**显式记 0**，不缺行。样本域与第六线逐字相同：
`{summary['attemptScope']['scopeRule']}`。

对账：面板总尝试 {panel['attemptsTotal']:,}、总技术失败 {panel['techFailsTotal']:,}
（与第六线样本域完全相等）。当日 ≥1 次尝试的桩日 {panel['sampleRowsWithAttempt']:,} 行，
其中 {panel['failDayRateAmongUsed']:.4f} 至少出现一次技术启动失败；0 次尝试的 {panel['zeroAttemptDays']} 行
失败率恒为 {panel['failDayRateAmongZero']:.1f}。

### ⚠ 暴露度：这份数据最容易误用的一点

当日失败日占比随当日尝试次数单调上升：

| 当日尝试数 | 失败日占比 |
|---|---|
{gradient}

因此 `attempts_on_day` / `users_on_day` / `started_on_day` 是**标签窗口内**的量，训练时点（昨天为止）
不可得，已写进 `NON_FEATURE_COLUMNS` 禁入名单。本线只允许"截至昨天的日均尝试量"这类因果量，
并在报告里单列一条 oracle 基线（真用明天的真实尝试次数）标注为不可部署，用来读出"排序收益里
有多少是暴露度"。

## day_context.csv

每城 × 每日的日历事实（周末/场景事件/计划系数）与天气日聚合（气温均值/极值、湿度、雨量、
降雨小时数、主导天气）。注意：本文件存的是**当天事实**，不含因果判断；特征层只用"严格早于
当日起点"的量（天气取前一日聚合，日历是计划量可取当日），见
`data_analysis/ml/reliability/features.py`。电价表**不产出特征**（日级聚合后是城市级常量，
等于把禁入的 city_id 换马甲带进来）。

## 明确不读的表/列

{lines}

## 口径声明

{summary['disclaimer']}
"""


if __name__ == "__main__":
    main()
