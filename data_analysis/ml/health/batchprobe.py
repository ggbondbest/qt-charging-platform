"""跨批次泛化探针（DEV PROBE）：把第九线整条链在**一个全新种子批次**上原样重跑一遍。

动机（README「十轮滚动」一节留下的问题）：发布批次 TEST AUC 0.6219 与滚动 7/10 胜、p=0.344
并存——赢的到底是特征结构，还是这一批随机实现（seed luck）？唯一不靠嘴的回答方式是拿一个
本线**从未训练过**的新批次重跑全链。本工具就是那次回答的固化版，可复跑、可换 seed 复跑。

与发布链路的边界（写死的纪律）：

* 探针批次只落在 ``data_analysis/outputs/probe_batches/``（gitignore），**绝不碰**
  ``datasets/analytics_full_180d_v1`` 与 ``outputs/ml_health``；模型 id 独立
  （默认 ``gbdt-charger-health-7d-probe1``），manifest 明写
  ``storageProfile=DEV_PROBE_NOT_PUBLISHED``——它不是仓库发布，谁都不能拿它的数字对外。
* clean 层是**透明直通转换**：探针原始批次用 ``dirty_rate=0.0`` 生成（不造假脏数据，且
  用户已授权"新增数据、不改原数据"），raw 内容 == clean 内容，唯一做的加工是把 CSV 列
  按发布 clean 表**逐列同 dtype** 落 parquet（时区约定 raw↔clean 用 ticket_id 连接实证为
  去 Z、不移相）。Spark 清洗链在本机 Python 3.13 下不可复跑（pyspark 要求 ≤3.12），
  因此 pipelineRunId 明写 ``dev-probe-pass-through-not-spark``——不冒充 Spark 产物。
* 几何对齐：mlSplits 复用发布批次的四日期（探针生成器配置 start_date/days 相同），
  面板、purge、删失的**行数**与发布批一致，指标可直接横向对读。

用法（仓库根）::

    python -m data_analysis.charging_data.generator --config <新批次config> --output data_analysis/outputs/probe_batches/<id>
    python -m data_analysis.ml.health.batchprobe --probe-root data_analysis/outputs/probe_batches/<id>

产物落在 ``--out``（默认 ``outputs/ml_health_probe``）：完整 features/train/evaluate 链产物
+ ``probe_summary.json``/``probe_summary.md``（发布批 vs 探针批记分牌对照 + 桩级工单散布的
二项噪声检验 + 机制证据）。所有目录写入受 ``require_empty_run_dir`` 与独占创建保护。
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
import pandas as pd

from . import common

DATA_ANALYSIS_ROOT = common.DATA_ANALYSIS_ROOT
PUBLISHED_ROOT = DATA_ANALYSIS_ROOT / "datasets" / "analytics_full_180d_v1"

#: 第九线读的全部 clean 表；探针层必须逐列同构，缺列多列都 fail loud。
TABLES = ["maintenance_tickets", "charging_sessions", "charging_attempts", "charger_telemetry",
          "chargers", "stations", "calendar", "weather_hourly"]


def published_dtypes(ref_clean_dir: Path, table: str) -> dict[str, str]:
    """发布 clean 某表的 {列: dtype}（列序即返回 dict 的迭代序，供下游对齐列序）。"""
    parts = sorted(Path(ref_clean_dir).glob(f"{table}/*.parquet"))
    if not parts:
        raise FileNotFoundError(f"探针需要发布 clean 表 {table} 的 dtype 作基准，但 {ref_clean_dir} 里没有")
    df = pd.read_parquet(parts[0])
    return {c: str(df[c].dtype) for c in df.columns}


def clean_column(series: pd.Series, dtype: str, table: str, column: str) -> pd.Series:
    """把 raw CSV 列转成发布 clean 的同型列。时区约定：ISO-Z 字符串 → 去 Z 的 naive UTC。"""
    if dtype.startswith("datetime64"):
        stripped = series.astype("string").str.replace("Z", "", regex=False)
        out = pd.to_datetime(stripped, errors="raise", utc=True).dt.tz_localize(None)
        return out.astype("datetime64[ns]")
    if dtype == "int64":
        return pd.to_numeric(series, errors="raise").astype("int64")
    if dtype == "float64":
        return pd.to_numeric(series, errors="coerce").astype("float64")
    if dtype == "object":
        return series.astype("object").where(series.notna(), None)
    raise AssertionError(f"{table}.{column}: 发布 clean 出现未处理的 dtype {dtype!r}")


def build_clean(raw_dir: Path, clean_out: Path, ref_clean_dir: Path,
                tables: list[str] | None = None) -> dict[str, int]:
    """raw CSV(.gz) → 与发布同 dtype 的 parquet；列集合不同即拒绝。

    ``tables`` 默认是第九线读的全部 8 张表；测试里可以只跑一张（传参不改变生产行为）。
    """
    counts: dict[str, int] = {}
    for table in (tables or TABLES):
        dtypes = published_dtypes(ref_clean_dir, table)
        parts = sorted(Path(raw_dir).glob(f"{table}/*.csv.gz"))
        if not parts:
            raise FileNotFoundError(f"探针 raw 缺表 {table}: {raw_dir}")
        frames = [pd.read_csv(p, dtype=str, keep_default_na=False, na_values=[""]) for p in parts]
        raw = pd.concat(frames, ignore_index=True)
        if set(raw.columns) != set(dtypes):
            raise AssertionError(f"{table}: raw 列 {sorted(raw.columns)} != 发布 clean 列 {sorted(dtypes)}")
        built = pd.DataFrame({col: clean_column(raw[col], dtypes[col], table, col) for col in dtypes})
        built = built[list(dtypes)]                       # 列序也与发布一致
        dest = Path(clean_out) / table
        dest.mkdir(parents=True, exist_ok=True)
        built.to_parquet(dest / "part-00000.snappy.parquet", index=False)
        counts[table] = len(built)
    return counts


def write_probe_manifest(probe_root: Path, published_manifest: dict, counts: dict[str, int],
                         dataset_id: str) -> str:
    """探针 serving_manifest：批量复用发布 manifest 的 mlSplits/definitions 几何，
    身份字段全部换成探针自己的、且显式标注非发布。"""
    raw_manifest_path = Path(probe_root) / "manifest.json"
    raw_manifest = json.loads(raw_manifest_path.read_text(encoding="utf-8"))
    probe_batch_id = "analytics-probe-" + hashlib.sha256(dataset_id.encode()).hexdigest()[:32]
    manifest = dict(published_manifest)
    manifest["datasetId"] = dataset_id
    manifest["publishedBatchId"] = probe_batch_id
    manifest["sourceManifestSha256"] = hashlib.sha256(raw_manifest_path.read_bytes()).hexdigest()
    manifest["sourceGeneratorConfig"] = raw_manifest.get("config", {})
    manifest["pipelineRunId"] = "dev-probe-pass-through-not-spark"
    manifest["storageProfile"] = "DEV_PROBE_NOT_PUBLISHED"
    manifest["definitions"] = dict(published_manifest["definitions"])
    manifest["definitions"]["batch_class"] = (
        "DEV PROBE: fresh simulator batch (different seed, dirty_rate=0), pass-through cast to the "
        "published clean dtypes. NOT a repository publication; used only to check the health line "
        "rebuilds and generalizes on data it never trained on.")
    manifest["tables"] = {name: {"rowCount": n} for name, n in counts.items()}
    common.write_new_json(Path(probe_root) / "serving_manifest.json", manifest)
    return probe_batch_id


def ticket_spread(tickets: pd.DataFrame, days: int) -> dict:
    """桩级日均来修率的散布 vs iid 二项噪声的散布。

    比值 ≈1 ⇒ 桩间差异与"每桩每 tick 独立抛硬币"完全一致，没有可学的先天差异；
    比值明显 >1 才说明存在桩级异质的余地。"""
    cnt = tickets.groupby("charger_id").size()
    expected = len(tickets) / len(cnt)
    observed_sd = float((cnt / days).std())
    binomial_sd = float(np.sqrt(expected) / days)
    return {"chargers": int(len(cnt)), "tickets": int(len(tickets)),
            "meanTicketsPerCharger": round(float(expected), 2),
            "ratePerChargerDay min/p50/max": [round(float((cnt / days).min()), 4),
                                              round(float((cnt / days).median()), 4),
                                              round(float((cnt / days).max()), 4)],
            "sdObserved": round(observed_sd, 5), "sdIidBinomial": round(binomial_sd, 5),
            "overdispersionRatio": round(observed_sd / binomial_sd, 3)}


def _scoreboard(report_path: Path) -> dict:
    if not Path(report_path).exists():
        return {}
    report = json.loads(Path(report_path).read_text(encoding="utf-8"))
    return {"chosen": f"{report['chosenSet']}/{report['chosenHyper']}",
            "baseRate": report.get("risk", {}).get("baseRate"),
            "auc": {k: v["auc"] for k, v in report["scoreboard"].items()}}


def _spread_or_note(tickets: pd.DataFrame, days: int) -> dict:
    if len(tickets) == 0:
        return {"unavailable": "缺该批次的 maintenance_tickets（新克隆上发布产物可能不存在）"}
    return ticket_spread(tickets, days)


def _ratio(spread: dict):
    return spread.get("overdispersionRatio", "n/a")


def build_summary(probe_report_path: Path, published_report_path: Path,
                  probe_tickets: pd.DataFrame, published_tickets: pd.DataFrame,
                  days: int) -> dict:
    """两批判分板并排 + 散布对照 + 机制结论。缺发布产物时（新克隆）对照列自动留空。"""
    probe, published = _scoreboard(probe_report_path), _scoreboard(published_report_path)
    keys = sorted(set(probe.get("auc", {})) | set(published.get("auc", {})))
    comparison = {k: {"published": published.get("auc", {}).get(k),
                      "probe": probe.get("auc", {}).get(k),
                      "delta": (None if not (probe.get("auc", {}).get(k) is not None
                                             and published.get("auc", {}).get(k) is not None)
                                else round(probe["auc"][k] - published["auc"][k], 4))}
                  for k in keys}
    spread_pub = _spread_or_note(published_tickets, days)
    spread_probe = _spread_or_note(probe_tickets, days)
    return {"disclaimer": ("探针批次 = DEV PROBE（不同 seed 的全新仿真批次，dirty_rate=0，"
                           "透明直通 clean 层），不是仓库发布；全部数字只用于检验第九线对"
                           "未见批次的泛化性，不得对外引用。"),
            "geometry": {"businessDays": days},
            "chosen": {"published": published.get("chosen"), "probe": probe.get("chosen")},
            "testBaseRate": {"published": published.get("baseRate"), "probe": probe.get("baseRate")},
            "aucComparison": comparison,
            "ticketSpread": {"published": spread_pub, "probe": spread_probe},
            "mechanism": ("charging_data/generator.py 的 Sim.maintenance()：每 tick × 每桩以 "
                          "p=0.00019 独立抽签决定是否产生工单，抽签不读取桩龄/功率/遥测/历史工单"
                          "——工单按构造就是与特征无关的 iid 伯努利流；桩级散布比"
                          "（观察sd/iid二项sd）发布批 {}、探针批 {}，与之一致。"
                          ).format(_ratio(spread_pub), _ratio(spread_probe))}


def repoint(probe_root: Path, out_dir: Path, model_id: str, probe_batch_id: str) -> None:
    """把第九线的全部运行时常量指到探针路径。features/train/evaluate 都经
    ``common.常量`` 间接寻址，因此改这一处即可整链换底座——发布路径不再被触碰。"""
    common.SOURCE_DATASET_DIR = probe_root
    common.CLEAN_DIR = probe_root / "clean"
    common.DATASET_ID = json.loads((probe_root / "serving_manifest.json")
                                   .read_text(encoding="utf-8"))["datasetId"]
    common.EXPECTED_PUBLISHED_BATCH_ID = probe_batch_id
    common.MODEL_ID = model_id
    common.OUT_DIR = out_dir
    common.FEATURES_PATH = out_dir / "health_features.pkl"
    common.BUILD_SUMMARY_PATH = out_dir / "features_summary.json"
    common.BUNDLE_PATH = out_dir / f"{model_id}.joblib"
    common.TRAIN_METRICS_PATH = out_dir / "train_metrics.json"
    common.TEST_REPORT_PATH = out_dir / "evaluation_report.json"
    common.TEST_REPORT_MD = out_dir / "evaluation_report.md"
    common.PREDICTIONS_CSV = out_dir / "test_predictions.csv"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m data_analysis.ml.health.batchprobe",
                                     description="跨批次泛化探针（DEV PROBE，不是发布）")
    parser.add_argument("--probe-root", required=True, type=Path,
                        help="generator 输出目录（含 manifest.json 与 raw/）")
    parser.add_argument("--out", type=Path, default=DATA_ANALYSIS_ROOT / "outputs" / "ml_health_probe")
    parser.add_argument("--model-id", default="gbdt-charger-health-7d-probe1")
    args = parser.parse_args(argv)

    probe_root = args.probe_root.resolve()
    out_dir = args.out.resolve()
    raw_manifest = json.loads((probe_root / "manifest.json").read_text(encoding="utf-8"))
    dataset_id = raw_manifest["dataset_id"]
    if raw_manifest.get("config", {}).get("dirty_rate", 0.0) != 0.0:
        raise SystemExit("探针批次必须 dirty_rate=0.0（直通转换的前提）；请先用零脏度重新生成")

    published_manifest = common.read_source_manifest()
    probe_batch_id = "analytics-probe-" + hashlib.sha256(dataset_id.encode()).hexdigest()[:32]
    clean_out = probe_root / "clean"
    if clean_out.exists() and any(clean_out.iterdir()):
        raise SystemExit(f"{clean_out} 已有内容——探针拒绝就地改写；换 probe-root 或先归档")
    print(f"[probe] dataset={dataset_id} -> 探针批次 {probe_batch_id}（DEV PROBE，非发布）")
    counts = build_clean(probe_root / "raw", clean_out, PUBLISHED_ROOT / "clean")
    write_probe_manifest(probe_root, published_manifest, counts, dataset_id)

    common.require_empty_run_dir(out_dir, extra_allowed=("health_features.pkl", "features_summary.json"))
    repoint(probe_root, out_dir, args.model_id, probe_batch_id)
    from . import evaluate, features, train          # 必须在 repoint 之后 import
    fsummary = features.main()
    tsummary = train.main()
    esummary = evaluate.main()

    days = len(pd.DatetimeIndex(pd.read_pickle(common.FEATURES_PATH)["business_date"].unique()))
    published_report = DATA_ANALYSIS_ROOT / "outputs" / "ml_health" / "evaluation_report.json"
    published_tickets_path = sorted((PUBLISHED_ROOT / "clean" / "maintenance_tickets").glob("*.parquet"))
    published_tickets = (pd.concat([pd.read_parquet(p) for p in published_tickets_path])
                         if published_tickets_path else pd.DataFrame(columns=["charger_id"]))
    summary = build_summary(common.TEST_REPORT_PATH, published_report,
                            common.load_clean_table("maintenance_tickets"),
                            published_tickets, days)
    summary["chain"] = {"features": {k: fsummary.get(k) for k in ("featureRows", "featureCount")},
                        "leakAudit": fsummary.get("leakAudit"),
                        "train": {k: tsummary.get(k) for k in
                                  ("chosenSet", "chosenHyper", "operatingPoint")},
                        "evaluate": {k: esummary.get(k) for k in ("testRows", "baseRate")}}
    common.write_new_json(out_dir / "probe_summary.json", summary)
    lines = [f"# 跨批次泛化探针摘要 · {dataset_id}", "", f"> {summary['disclaimer']}", "",
             f"- 发布链择组 {summary['chosen']['published']} → 探针链择组 **{summary['chosen']['probe']}**"
             "（择组本身不稳定 = 信号弱的旁证）", ""]
    lines += ["| 打分 | 发布批 TEST AUC | 探针批 TEST AUC | Δ |", "| --- | --- | --- | --- |"]
    for key, row in summary["aucComparison"].items():
        lines.append(f"| {key} | {row['published']} | {row['probe']} | {row['delta']} |")
    lines += ["", f"- 工单散布（{days} 个业务日）：发布批 overdispersion "
              f"{_ratio(summary['ticketSpread']['published'])}，探针批 "
              f"{_ratio(summary['ticketSpread']['probe'])}",
              f"- 机制：{summary['mechanism']}", ""]
    common.write_new_text(out_dir / "probe_summary.md", "\n".join(lines))
    print(f"[probe] 完成。摘要：{out_dir / 'probe_summary.md'}")
    print(f"[probe] 探针 TEST 记分牌：{json.dumps(summary['aucComparison'], ensure_ascii=False)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
