"""对 TEST 段做一次性盲测：下一 tick 负荷模型 vs 三个可部署基线，写 JSON+MD 报告。

纪律：模型、persistence/climatology/global 基线**全部只在 TEST 上打分一次**；climatology 用 bundle
里冻结的 TRAIN 查表（不在 TEST 上重算），与 train.py 完全同口径。附带分布尾部读数——分配关心的是
**尖峰**，故除整体 RMSE 外单列"最高 10% 负荷 tick"上的误差，以及"实测越 360kW 的 tick 数"的复核。

用法（仓库根目录）：python -m data_analysis.ml.transformer.evaluate
"""

from __future__ import annotations

import json

import joblib
import numpy as np

from . import common
from .train import load_matrix, usable, design, predict_climatology, _metrics


def _tail_metrics(y: np.ndarray, pred: np.ndarray) -> dict:
    block = _metrics(y, pred)
    hi = np.quantile(y, 0.90)
    mask = y >= hi
    block["maeTopDecile"] = round(float(np.mean(np.abs(pred[mask] - y[mask]))), 4) if mask.any() else None
    block["rmseTopDecile"] = round(float(np.sqrt(np.mean((pred[mask] - y[mask]) ** 2))), 4) if mask.any() else None
    block["bias"] = round(float(np.mean(pred - y)), 4)
    return block


def main() -> dict:
    common.require_empty_run_dir(
        common.OUT_DIR,
        extra_allowed=(common.TICK_FEATURES_PATH.name, common.TICK_SUMMARY_PATH.name,
                       common.DEMAND_LONG_PATH.name, common.BUNDLE_PATH.name,
                       common.TRAIN_METRICS_PATH.name))
    common.verify_batch()
    frame, summary = load_matrix()
    with open(common.TRAIN_METRICS_PATH, encoding="utf-8") as handle:
        train_metrics = json.load(handle)
    bundle = joblib.load(common.BUNDLE_PATH)
    if bundle["featuresSha256"] != summary["featuresSha256"]:
        raise common.BatchMismatch("bundle 训练时的特征哈希与当前特征表不一致，模型不可信")

    columns = bundle["featureNames"]
    test = usable(frame, "TEST").reset_index(drop=True)
    y = test["total_kw_next"].to_numpy(dtype=float)
    x = design(test, columns)

    model_pred = bundle["regressor"].predict(x)
    clim = bundle["climatology"]
    scores = {
        "model": model_pred,
        "globalMean": np.full(len(test), float(clim["global"])),
        "persistence": test["load_lag1"].to_numpy(dtype=float),
        "climatology": predict_climatology(clim, test),
    }
    results = {name: _tail_metrics(y, pred) for name, pred in scores.items()}
    persistence_rmse = results["persistence"]["rmse"]
    model_rmse = results["model"]["rmse"]

    observed_over = int((test["total_kw"].to_numpy() > common.TRANSFORMER_KW).sum())
    predicted_over = int((model_pred > common.TRANSFORMER_KW).sum())
    peak = float(test["total_kw"].max())

    report = {
        "publishedBatchId": summary["publishedBatchId"], "pipelineRunId": summary["pipelineRunId"],
        "datasetId": common.DATASET_ID, "modelId": common.MODEL_ID, "seed": common.SEED,
        "featuresSha256": summary["featuresSha256"], "bundleSha256": common.sha256_file(common.BUNDLE_PATH),
        "sampleUnit": summary["sampleUnit"], "label": summary["label"],
        "testRows": int(len(test)), "results": results,
        "headline": {
            "modelRmseVsPersistencePct": round(100.0 * (1.0 - model_rmse / persistence_rmse), 2),
            "observedTicksOverCap": observed_over, "predictedTicksOverCap": predicted_over,
            "maxObservedStationTickKw": round(peak, 2), "transformerKw": common.TRANSFORMER_KW,
            "note": ("TEST 段实测站·tick 总负荷从不越 360kW，与全批一致（过载是负事实）；"
                     "模型的用处是把余量算准，不是预测越限"),
        },
        "trainSelectionRule": train_metrics.get("selectionRule"),
        "chosenHyper": bundle["chosenHyper"],
        "disclaimer": common.data_note(),
    }
    common.write_new_json(common.TEST_REPORT_PATH, report)
    common.write_new_text(common.TEST_REPORT_MD, _markdown(report))

    print(f"[evaluate] TEST={len(test):,} · 模型 RMSE={model_rmse:.3f} "
          f"vs persistence {persistence_rmse:.3f}（改善 {report['headline']['modelRmseVsPersistencePct']}%）")
    for name, block in results.items():
        print(f"[evaluate]   {name}: MAE={block['mae']:.3f} RMSE={block['rmse']:.3f} "
              f"topDecileMAE={block['maeTopDecile']} bias={block['bias']}")
    print(f"[evaluate] 实测越 360kW tick={observed_over} 预测越={predicted_over} 峰值={peak:.1f}kW")
    print(f"[evaluate] -> {common.TEST_REPORT_MD}")
    return report


def _markdown(report: dict) -> str:
    lines = ["# 站×tick 下一负荷预测 · TEST 盲测报告", ""]
    lines.append(f"> {report['disclaimer']}")
    lines.append("")
    lines.append(f"- 样本：{report['testRows']:,} 个 TEST 站·tick · 模型 {report['modelId']} · "
                 f"容量档 {report['chosenHyper']}")
    lines.append(f"- 结论：模型 RMSE 较 persistence 基线改善 **{report['headline']['modelRmseVsPersistencePct']}%**；"
                 f"TEST 段实测越 {report['headline']['transformerKw']:.0f}kW 的 tick = "
                 f"{report['headline']['observedTicksOverCap']}（{report['headline']['note']}）")
    lines.append("")
    lines.append("| 打分 | MAE | RMSE | 最高10%tick MAE | 偏差 |")
    lines.append("| --- | ---: | ---: | ---: | ---: |")
    for name, block in report["results"].items():
        lines.append(f"| {name} | {block['mae']} | {block['rmse']} | {block['maeTopDecile']} | {block['bias']} |")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    main()
