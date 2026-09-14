"""Write out a human-readable forecast table: what the shipped model predicts, hour by hour.

    python -m data_analysis.ml.availability.forecast_table \
        --run-dir data_analysis/outputs/ml_avail_run2 --reference-hours 6

This is a delivery/inspection tool, not a serving path.  It drives the *same* entry points the
backend adapter would call - :class:`AvailabilityForecaster.predict` for the contracted integer
points and :meth:`AvailabilityForecaster.risk` for the distribution, interval and depletion
probability - over a block of TEST reference times and every station, then joins each answer to the
label the batch already holds for that station hour.  The point is to be able to look at rows:

``forecasts.csv``  one row per (bundle, station, reference time, step): predicted whole chargers,
                   central interval, expected chargers, P(no charger), the actual count, the error.
``forecast_table.md``  a digest a person can read: per-horizon and per-station error over the
                   window, one station's full 24-hour curve, and a station x hour "predicted/actual"
                   grid for the finest horizon.

Nothing is fitted or selected here.  Values come from the bundles as shipped, so a table whose
errors disagree with ``model_metadata.json``'s MAE would be a bug in one of the two - which is why
this file sits next to the evaluation report instead of inside it.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import pandas as pd

from data_analysis.contracts.model import PredictionContext
from data_analysis.ml.availability import predict as serving
from data_analysis.ml.common import artifacts, forecaster
from data_analysis.ml.common.data_io import DEFAULT_EXPORT
from data_analysis.ml.common.tasks import AVAILABILITY

SIMULATED = "全部指标为模拟数据测试结果（第二阶段发布批次），不代表真实运营数据表现。"


def _iso(moment: pd.Timestamp) -> str:
    return pd.Timestamp(moment).strftime("%Y-%m-%dT%H:%M:%SZ")


def _forecast_rows(forecasters: dict[int, serving.AvailabilityForecaster], frame,
                   stations: list[str], moments: list[pd.Timestamp]) -> pd.DataFrame:
    """Serve every (bundle, station, reference time) and join the batch's own answer labels.

    The label join is a narrow pre-filtered lookup, not a per-request scan of the frame: the only
    columns taken out of ``frame.data`` are the split flags of the three horizons and the 24 answer
    labels, keyed on the same ``(station, reference_time)`` pair the request was made for.
    """
    export = frame.export
    references = [_iso(moment) for moment in moments]
    needed = (["station_id", "reference_time"]
              + [export.split_column(horizon) for horizon in (1, 6, 24)]
              + [AVAILABILITY.label_column(step) for step in range(1, 25)])
    window = frame.data.loc[frame.data["reference_time"].map(_iso).isin(references), needed].copy()
    window["_ref"] = window["reference_time"].map(_iso)
    lookup = window.set_index(["station_id", "_ref"])
    records: list[dict] = []
    for horizon, entry in sorted(forecasters.items()):
        bundle, metadata = entry.bundle, entry.metadata
        split_column = export.split_column(horizon)
        for station_id in stations:
            for moment, reference in zip(moments, references):
                key = (station_id, reference)
                if key not in lookup.index:
                    continue
                row = lookup.loc[key]
                if row[split_column] != "TEST":
                    continue  # outside the block this model was ever scored on
                history = serving._window(frame.hourly, station_id, moment)
                context = PredictionContext(
                    dataset_id=export.dataset_id, published_batch_id=export.published_batch_id,
                    station_id=station_id, reference_time=reference, horizon_hours=horizon,
                    model_id=metadata["modelId"],
                )
                points = entry.predict(history, context)["points"]
                risk = entry.risk(history, context)["hours"]
                profile = bundle["stations"][station_id]
                site_keys = bundle.get("stationSiteKeys") or {}
                for step, (point, hour) in enumerate(zip(points, risk), start=1):
                    label = row[AVAILABILITY.label_column(step)]
                    if label is None or (isinstance(label, float) and math.isnan(label)):
                        raise ValueError(f"{station_id} {reference}: no label for step {step}")
                    records.append({
                        "modelId": metadata["modelId"], "horizonHours": horizon,
                        "station_id": station_id, "city_id": profile["city_id"],
                        "site_type": site_keys.get(station_id) or serving.site_key_of(profile),
                        "capacity": int(profile["capacity"]),
                        "reference_time": reference, "answer_time": point["timestamp"], "step": step,
                        "predicted_chargers": int(point["value"]),
                        "interval_low": int(hour["intervalLow"]), "interval_high": int(hour["intervalHigh"]),
                        "expected_chargers": float(hour["expectedChargers"]),
                        "p_no_charger": float(hour["probabilityDepleted"]),
                        "distribution": json.dumps(hour["distribution"], ensure_ascii=False),
                        "actual_chargers": int(label),
                        "abs_error": abs(int(label) - int(point["value"])),
                        "within_interval": int(hour["intervalLow"] <= int(label) <= hour["intervalHigh"]),
                    })
    return pd.DataFrame.from_records(records)


def _markdown(table: pd.DataFrame, arguments) -> str:
    lines = ["# 空闲桩预测表（出厂模型，默认 `ml_avail_run2` / 0.3.0）", "", SIMULATED, "",
             f"- 复现命令：`{table.attrs['command']}`",
             f"- 参考时刻 {table['reference_time'].nunique()} 个："
             f"{table['reference_time'].min()} … {table['reference_time'].max()}",
             f"- 站点 {table['station_id'].nunique()} 个；{len(table)} 行"
             f"（每行 = 一个 bundle × 一站 × 一个参考时刻 × 一个未来小时）",
             "- 单位 = 空闲充电桩个数（对外整数）；`expected_chargers` 是分布期望，**不是库存**，"
             "展示时必须标注为预计值", "", "## 1. 这一段窗口的误差（按跨度）", "",
             "| 跨度 | 点数 | MAE | 完全命中 | ±1 以内 | 区间覆盖率 |", "| --- | --- | --- | --- | --- | --- |"]
    for horizon, block in table.groupby("horizonHours"):
        lines.append(f"| h{int(horizon):02d} | {len(block)} | {block['abs_error'].mean():.4f} | "
                     f"{(block['abs_error'] == 0).mean():.1%} | {(block['abs_error'] <= 1).mean():.1%} | "
                     f"{block['within_interval'].mean():.1%} |")
    lines += ["", "> **这一行不能当模型准确率**：它只覆盖上面列出的那几个参考时刻（每行 = 一站一小时），"
              "样本量是几十到几千点，而交付口径的 MAE 是全体 TEST 18,000 / 107,250 / 418,200 点，"
              "看 `ml_avail_eval_run2_v2/evaluation_report.md` 或各 bundle 的 `model_metadata.json`。"
              "本表的作用是**逐行核对预测是否合法、是否离谱**，不是评分。"]
    finest = table[table["horizonHours"] == table["horizonHours"].min()]
    lines += ["", "## 2. 分站点误差（最细跨度 h01）", "",
              "| 站点 | 城市 | 场站类型 | 点数 | MAE | 完全命中 | 预测=0 次数 | 实际=0 次数 |",
              "| --- | --- | --- | --- | --- | --- | --- | --- |"]
    for station_id, block in finest.groupby("station_id"):
        lines.append(f"| {station_id} | {block['city_id'].iloc[0]} | {block['site_type'].iloc[0]} | "
                     f"{len(block)} | {block['abs_error'].mean():.3f} | "
                     f"{(block['abs_error'] == 0).mean():.1%} | "
                     f"{int((block['predicted_chargers'] == 0).sum())} | "
                     f"{int((block['actual_chargers'] == 0).sum())} |")
    longest = table[table["horizonHours"] == table["horizonHours"].max()]
    sample_station = arguments.sample_station or sorted(finest["station_id"].unique())[0]
    curve = longest[(longest["station_id"] == sample_station)
                    & (longest["reference_time"] == longest["reference_time"].min())]
    lines += ["", f"## 3. 一条完整的 24 小时曲线（{sample_station} @ "
              f"{curve['reference_time'].iloc[0] if len(curve) else 'n/a'}）", "",
              "| 未来时刻 | 预测桩数 | 80% 区间 | 期望(预计值) | P(无桩) | 实际 | 误差 |",
              "| --- | --- | --- | --- | --- | --- | --- |"]
    for _, row in curve.iterrows():
        lines.append(f"| {row['answer_time']} | {row['predicted_chargers']} | "
                     f"[{row['interval_low']}, {row['interval_high']}] | {row['expected_chargers']:.3f} | "
                     f"{row['p_no_charger']:.3f} | {row['actual_chargers']} | {row['abs_error']} |")
    hours = sorted(finest["reference_time"].unique())[:arguments.display_hours]
    lines += ["", f"## 4. h01 逐站逐时「预测/实际」（前 {len(hours)} 个参考时刻）", "",
              "| 站点 | " + " | ".join(hour[11:16] + "Z" for hour in hours) + " |",
              "| --- | " + " | ".join("---" for _ in hours) + " |"]
    for station_id, block in finest[finest["reference_time"].isin(hours)].groupby("station_id"):
        by_hour = block.set_index("reference_time")
        lines.append(f"| {station_id} | " + " | ".join(
            f"{int(by_hour.loc[hour, 'predicted_chargers'])}/{int(by_hour.loc[hour, 'actual_chargers'])}"
            for hour in hours) + " |")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--export", default=str(DEFAULT_EXPORT))
    parser.add_argument("--run-dir", default="data_analysis/outputs/ml_avail_run2")
    parser.add_argument("--output", required=True)
    parser.add_argument("--reference-hours", type=int, default=6,
                        help="how many of the latest TEST reference times to serve")
    parser.add_argument("--display-hours", type=int, default=8, help="columns in the md grid")
    parser.add_argument("--station", action="append", default=[], help="repeat to narrow; default all")
    parser.add_argument("--sample-station", default=None, help="station whose 24h curve is printed")
    arguments = parser.parse_args(argv)

    output = Path(arguments.output)
    if output.exists() and any(output.iterdir()):
        raise SystemExit(f"{output} is not empty; pass a new --output directory")
    forecasters: dict[int, serving.AvailabilityForecaster] = {}
    for directory in artifacts.bundle_directories(Path(arguments.run_dir)):
        entry = serving.AvailabilityForecaster.load(directory)
        if entry.bundle.get("excludeCity"):
            continue  # a cold-start model is not the serving path for the city it never saw
        forecasters.setdefault(entry.horizon_hours, entry)
    if not forecasters:
        raise SystemExit(f"{arguments.run_dir} holds no non-holdout bundles")

    frame = forecaster.build_frame(Path(arguments.export))
    # Reference times are taken from the TEST block of the *longest* horizon being served: longer
    # horizons lose their tail rows (the answer window would run past the split), so a moment that
    # is TEST for h24 is TEST for h01 and h06 as well, and one table then covers every bundle.
    moment_split = frame.export.split_column(max(forecasters))
    test_rows = frame.data.loc[frame.data[moment_split] == "TEST"]
    moments = [pd.Timestamp(value) for value in sorted(test_rows["reference_time"].unique())]
    moments = moments[-arguments.reference_hours:]
    stations = arguments.station or sorted(test_rows["station_id"].unique())
    print(f"[serve] horizons {sorted(forecasters)} x {len(stations)} stations x "
          f"{len(moments)} reference times")
    table = _forecast_rows(forecasters, frame, stations, moments)
    table.attrs["command"] = artifacts.invocation(__name__, argv)
    output.mkdir(parents=True, exist_ok=True)
    table.to_csv(output / "forecasts.csv", index=False, encoding="utf-8-sig")
    (output / "forecast_table.md").write_text(_markdown(table, arguments), encoding="utf-8")
    summary = {
        "command": table.attrs["command"], "runDir": arguments.run_dir, "rows": int(len(table)),
        "caveat": SIMULATED,
        "window": {"maeByHorizon": {
            f"h{int(horizon):02d}": round(float(block["abs_error"].mean()), 4)
            for horizon, block in table.groupby("horizonHours")},
            "note": "只在这一段参考时刻上算，样本量远小于评估报告的全体 TEST，勿与 model_metadata.json 的 MAE 混用"},
    }
    (output / "forecast_summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"[done] {output}  {len(table)} rows  " + "  ".join(
        f"{key}={value:.4f}" for key, value in summary["window"]["maeByHorizon"].items()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
