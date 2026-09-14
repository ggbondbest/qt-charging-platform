"""一次性盲测：在 split == TEST 上评白排风险与等待时长，产出 JSON + Markdown 报告。

纪律：阈值、早停、基线表全部由 train.py 在 TRAIN/VALIDATION 上定死并冻进 bundle，本脚本
**只读不算新参数**；消融用的三个缩减模型也只是重新拟合后放在一起报数，不回头改主模型。
模型包与特征矩阵按 sha256 成对校验，批次错配一律 BatchMismatch 拒评。
所有数字都带 common.simulated_note() 那句口径声明。

用法（仓库根目录）：python -m data_analysis.ml.queue.evaluate
"""

from __future__ import annotations

import hashlib
import json

import joblib
import numpy as np
import pandas as pd
from sklearn.ensemble import HistGradientBoostingClassifier
from sklearn.metrics import average_precision_score, log_loss, mean_absolute_error, roc_auc_score

from . import common
from .train import design, predict_baseline, predict_wait

#: "队列/桩动态"这一消融组的列名前缀（其余数值列归到静态组）。
DYNAMIC_PREFIXES = ("arrivals_", "open_now", "wait_mean_", "wait_obs_", "waste_rate_",
                    "waste_n_", "station_history_n", "user_", "days_since_last", "is_first_queue",
                    "charger_", "available_share", "charging_power_")

HOUR_BANDS = (("凌晨0-6", 0, 6), ("白天6-17", 6, 17), ("晚峰17-22", 17, 22), ("夜间22-24", 22, 24))


def load_bundle() -> dict:
    if not common.BUNDLE_PATH.exists():
        raise FileNotFoundError(f"缺少模型包 {common.BUNDLE_PATH}，请先跑 train")
    bundle = joblib.load(common.BUNDLE_PATH)
    common.verify_batch()
    if bundle["publishedBatchId"] != common.EXPECTED_PUBLISHED_BATCH_ID:
        raise common.BatchMismatch(
            f"模型包绑定批次 {bundle['publishedBatchId']!r} 与当前数据层不一致")
    with open(common.MATRIX_PATH, "rb") as handle:
        digest = hashlib.sha256(handle.read()).hexdigest()
    if digest != bundle["matrixSha256"]:
        raise common.BatchMismatch(
            f"特征矩阵哈希 {digest[:12]}… != 模型包记录 {bundle['matrixSha256'][:12]}…，"
            f"模型与特征必须成对发布，请一起重跑 build_data + train")
    return bundle


def ablation(frame: pd.DataFrame, bundle: dict, test: pd.DataFrame, y: np.ndarray) -> dict[str, float]:
    """信号来自哪里：仅排位 / 仅静态 / 仅动态 / 全量，各在 TRAIN 重拟合后同评一次 TEST。"""
    numeric, categorical = bundle["numericFeatures"], bundle["categoricalFeatures"]
    dynamic = [n for n in numeric if n.startswith(DYNAMIC_PREFIXES)]
    static = [n for n in numeric if not n.startswith(DYNAMIC_PREFIXES) and n != "position_at_join"]
    train = frame[frame["split"] == "TRAIN"]
    sets = {
        "positionOnly": ([], []),
        "staticOnly": (static, categorical),
        "dynamicsOnly": (dynamic, []),
        "full": (numeric, categorical),
    }
    out: dict[str, float] = {}
    for label, (num, cat) in sets.items():
        names = (["position_at_join"] if label == "positionOnly" else []) + num + cat
        x_train, mask = design(train, [n for n in names if n not in cat], cat)
        x_test, _ = design(test, [n for n in names if n not in cat], cat)
        model = HistGradientBoostingClassifier(
            max_iter=400, learning_rate=0.06, max_leaf_nodes=31, min_samples_leaf=40,
            l2_regularization=1.0, early_stopping=True, validation_fraction=0.15,
            n_iter_no_change=30, random_state=common.SEED, categorical_features=mask)
        model.fit(x_train, train["y_waste"].to_numpy(dtype=int))
        out[label] = round(float(roc_auc_score(y, model.predict_proba(x_test)[:, 1])), 4)
        out[label + "_featureCount"] = int(x_train.shape[1])
    return out


def bucket_rows(test: pd.DataFrame, prob: np.ndarray, groups: list[tuple[str, np.ndarray]]) -> list[dict]:
    """按任意掩码分桶报 AUC；样本或正样本不足的桶如实标 null，不硬算一个好看的数。"""
    rows = []
    for label, mask in groups:
        y = test.loc[mask, "y_waste"].to_numpy(dtype=int)
        scores = prob[mask]
        if len(y) < 50 or y.sum() < 5 or (len(y) - y.sum()) < 5:
            rows.append({"bucket": label, "n": int(len(y)), "baseRate": round(float(y.mean()), 4)
                         if len(y) else None, "meanProb": round(float(scores.mean()), 4)
                         if len(y) else None, "auc": None, "note": "样本或正样本不足，AUC 不报"})
            continue
        rows.append({"bucket": label, "n": int(len(y)), "baseRate": round(float(y.mean()), 4),
                     "meanProb": round(float(scores.mean()), 4),
                     "auc": round(float(roc_auc_score(y, scores)), 4), "note": ""})
    return rows


def build_breakdowns(test: pd.DataFrame, prob: np.ndarray, wait_hat: np.ndarray) -> dict[str, list[dict]]:
    positions = [(f"排位{int(p)}", (test["position_at_join"] == p).to_numpy())
                 for p in sorted(test["position_at_join"].unique())]
    sites = [(str(k), (test["site_type"] == k).to_numpy())
             for k in sorted(test["site_type"].dropna().unique())]
    bands = [(label, ((test["hour_local"] >= lo) & (test["hour_local"] < hi)).to_numpy())
             for label, lo, hi in HOUR_BANDS]
    middle = test["business_date"].median()
    halves = [("TEST 前半月", (test["business_date"] < middle).to_numpy()),
              ("TEST 后半月", (test["business_date"] >= middle).to_numpy())]
    served = (test["y_waste"] == 0).to_numpy()
    rows = []
    for label, mask in positions:
        truth = test.loc[mask & served, "wait_min"].to_numpy(dtype=float)
        predicted = wait_hat[mask & served]
        if len(truth) < 30:
            rows.append({"bucket": label, "n": int(len(truth)), "labelMedian": None, "mae": None,
                         "note": "SERVED 样本不足 30，不报"})
            continue
        rows.append({"bucket": label, "n": int(len(truth)),
                     "labelMedian": round(float(np.median(truth)), 2),
                     "mae": round(float(mean_absolute_error(truth, predicted)), 4), "note": ""})
    return {"position": bucket_rows(test, prob, positions),
            "siteType": bucket_rows(test, prob, sites),
            "hourBand": bucket_rows(test, prob, bands),
            "halfWindow": bucket_rows(test, prob, halves),
            "waitByPosition": rows}


def markdown(report: dict) -> str:
    """人读的评测报告，表格全部由 report 里的数字生成，不手抄。"""
    risk = report["abandonRisk"]
    point = risk["frozenOperatingPoint"]
    wait = report["waitMinutes"]
    ablation_block = report["ablation"]
    lines = [
        f"# 排队结果与等待预测 · TEST 盲测报告（{report['modelVersion']}）",
        "",
        "> " + report["disclaimer"],
        "",
        f"- 模型：`{report['modelId']}`（白排风险，二分类） + `{report['waitModelId']}`（等待分钟，回归）",
        f"- 绑定批次：`{report['publishedBatchId']}` · run `{report['pipelineRunId']}`",
        f"- 特征矩阵 sha256：`{report['matrixSha256'][:16]}…` · 泄漏审计抽样 "
        f"{report['leakAudit']['sampled']} 行，四项 as-of 特征最大偏差 "
        f"{max(report['leakAudit']['maxAbsDiff'].values()):.1e}",
        f"- TEST 窗口（UTC）：`{report['testWindow']['validationEndExclusive']}` → "
        f"`{report['testWindow']['testEndExclusive']}`，样本 {report['testRows']:,} 次排队",
        "",
        "## 1. 白排风险（这次排队会不会白排）",
        "",
        "| 指标 | 模型 | 最强基线（三级平滑经验率） | 只看排位 |",
        "| --- | --- | --- | --- |",
        f"| AUC | **{risk['auc']:.4f}** | {risk['aucStrongBaseline']:.4f} | "
        f"{risk['aucPositionOnlyBaseline']:.4f} |",
        "",
        f"- 基础白排率 {risk['baseRate']:.4f}（不建模的下限）；PR-AUC {risk['prAuc']:.4f}；"
        f"Brier {risk['brier']:.4f}；LogLoss {risk['logLoss']:.4f}",
        f"- Top10% 提升 {risk['liftAt10pct']:.2f}×，Top20% 提升 {risk['liftAt20pct']:.2f}×",
        f"- 相对最强基线 AUC **{risk['aucGainVsStrongBaselinePct']:+.2f}%**，"
        f"相对只看排位 {risk['aucGainVsPositionOnlyPct']:+.2f}%",
        "",
        "冻结运营阈值（VALIDATION 上按精确率≥0.50 选定，TEST 只验不改）：",
        "",
        "| 阈值 | 告警率 | 精确率 | 召回 | F1 |",
        "| --- | --- | --- | --- | --- |",
        f"| {point['threshold']:.4f} | {point['alertRate']:.2%} | {point['precision']:.4f} | "
        f"{point['recall']:.4f} | {point['f1']:.4f} |",
        "",
        "## 2. 等待时长（还要等多久，单位分钟）",
        "",
        f"口径：{wait['evalSubset']}，n={wait['rows']:,}；标签 p50={wait['labelP50']} / "
        f"p90={wait['labelP90']} 分钟。",
        "",
        "| 方案 | MAE(分钟) |",
        "| --- | --- |",
        f"| **本模型 GBDT(L1)** | **{wait['modelMae']:.4f}** |",
        f"| 站点×小时中位数 | {wait['baselineStationHourMedianMae']:.4f} |",
        f"| 排位中位数 | {wait['baselinePositionMedianMae']:.4f} |",
        f"| 全局中位数 | {wait['baselineGlobalMedianMae']:.4f} |",
        "",
        f"- RMSE {wait['modelRmse']:.4f} 分钟；p90 绝对误差 {wait['p90AbsError']} 分钟；"
        f"±2 分钟命中率 {wait['within2MinRate']:.2%}",
        f"- 相对最优基线 MAE 降低 **{wait['maeGainVsBestBaselinePct']:.2f}%**；"
        f"输出区间合法（0–120 分钟）：{wait['outputRangeLegal']}",
        "",
        "## 3. 校准（预测概率 vs 实测频率，TEST 十分位）",
        "",
        "| 预测概率区间 | 样本数 | 预测均值 | 实测白排率 |",
        "| --- | --- | --- | --- |",
    ]
    for row in report["calibration"]:
        lines.append(f"| {row['bin']} | {row['n']:,} | {row['predicted']:.4f} | {row['observed']:.4f} |")
    lines += ["", "## 4. 信号来自哪里（同一 TEST 上的消融 AUC，缩减模型只是归因、不参与发布）", "",
              "| 特征组 | 特征数 | TEST AUC |", "| --- | --- | --- |"]
    labels = {"full": "全量（发布的主模型）", "positionOnly": "只有排位 position_at_join",
              "dynamicsOnly": "只有队列/桩动态 as-of", "staticOnly": "只有站点/用户/时间静态"}
    for key in ("positionOnly", "staticOnly", "dynamicsOnly", "full"):
        lines.append(f"| {labels[key]} | {ablation_block[key + '_featureCount']} | "
                     f"{ablation_block[key]:.4f} |")
    lines += ["", "## 5. 分桶表现", ""]
    for name, title in (("position", "按排位（风险）"), ("siteType", "按站点类型（风险）"),
                        ("hourBand", "按本地小时段（风险）"), ("halfWindow", "按 TEST 前后半月（风险）")):
        lines += [f"### {title}", "", "| 桶 | 样本 | 基础白排率 | 平均预测 | AUC | 备注 |",
                  "| --- | --- | --- | --- | --- | --- |"]
        for row in report["breakdown"][name]:
            auc = "—" if row["auc"] is None else f"{row['auc']:.4f}"
            base = "—" if row.get("baseRate") is None else f"{row['baseRate']:.4f}"
            mean = "—" if row.get("meanProb") is None else f"{row['meanProb']:.4f}"
            lines.append(f"| {row['bucket']} | {row['n']:,} | {base} | {mean} | {auc} | "
                         f"{row.get('note', '')} |")
        lines.append("")
    lines += ["### 按排位的等待时长 MAE（分钟）", "", "| 桶 | SERVED 样本 | 标签中位数 | MAE | 备注 |",
              "| --- | --- | --- | --- | --- |"]
    for row in report["breakdown"]["waitByPosition"]:
        mae = "—" if row.get("mae") is None else f"{row['mae']:.3f}"
        median = "—" if row.get("labelMedian") is None else f"{row['labelMedian']}"
        lines.append(f"| {row['bucket']} | {row['n']:,} | {median} | {mae} | {row.get('note', '')} |")
    lines += ["", "## 6. 数据纪律", "", f"- {report['dataDiscipline']}",
              f"- 标签口径：{report['labelUnits']}",
              f"- 零方差列已在建表阶段剔除 {len(report['droppedConstant'])} 个："
              f"{', '.join(sorted(report['droppedConstant']))}",
              f"- 站点结构提示：{report['constantWhy']}",
              "", "> " + report["disclaimer"], ""]
    return "\n".join(lines)


def main() -> dict:
    common.require_empty_run_dir(extra_allowed=(common.MATRIX_PATH.name,
                                                common.BUILD_SUMMARY_PATH.name,
                                                common.BUNDLE_PATH.name,
                                                common.TRAIN_METRICS_PATH.name))
    bundle = load_bundle()
    with open(common.BUILD_SUMMARY_PATH, encoding="utf-8") as handle:
        summary = json.load(handle)
    frame = pd.read_pickle(common.MATRIX_PATH)
    test = frame[frame["split"] == "TEST"].reset_index(drop=True)
    train = frame[frame["split"] == "TRAIN"]
    y = test["y_waste"].to_numpy(dtype=int)

    x_test, _ = design(test, bundle["numericFeatures"], bundle["categoricalFeatures"])
    prob = bundle["classifier"].predict_proba(x_test)[:, 1]
    threshold = float(bundle["operatingPoint"]["threshold"])
    flag = prob >= threshold

    baseline_test = predict_baseline(bundle["baselines"], test)
    naive = test["position_at_join"].map(train.groupby("position_at_join")["y_waste"].mean()
                                         ).to_numpy(dtype=float)
    auc, auc_base, auc_position = (float(roc_auc_score(y, prob)),
                                   float(roc_auc_score(y, baseline_test)),
                                   float(roc_auc_score(y, naive)))
    precision = float(y[flag].mean())
    recall = float(flag[y == 1].sum() / max(1, int(y.sum())))
    report: dict[str, object] = {
        "modelId": bundle["modelId"], "waitModelId": bundle["waitModelId"],
        "modelVersion": bundle["modelVersion"], "publishedBatchId": bundle["publishedBatchId"],
        "pipelineRunId": bundle["pipelineRunId"], "matrixSha256": bundle["matrixSha256"],
        "testWindow": bundle["splits"], "testRows": int(len(test)),
        "leakAudit": summary["leakAudit"], "labelUnits": summary["labelUnits"],
        "droppedConstant": summary["droppedConstant"], "constantWhy": summary["constantWhy"],
        "abandonRisk": {
            "baseRate": round(float(y.mean()), 4), "auc": round(auc, 4),
            "aucStrongBaseline": round(auc_base, 4), "aucPositionOnlyBaseline": round(auc_position, 4),
            "aucGainVsStrongBaselinePct": round(100.0 * (auc / auc_base - 1.0), 2),
            "aucGainVsPositionOnlyPct": round(100.0 * (auc / auc_position - 1.0), 2),
            "prAuc": round(float(average_precision_score(y, prob)), 4),
            "brier": round(common.brier(y, prob), 4),
            "logLoss": round(float(log_loss(y, np.clip(prob, 1e-6, 1 - 1e-6))), 4),
            "liftAt10pct": round(common.lift_at(y, prob, 0.10), 4),
            "liftAt20pct": round(common.lift_at(y, prob, 0.20), 4),
            "frozenOperatingPoint": {
                "threshold": round(threshold, 6), "alertRate": round(float(flag.mean()), 4),
                "precision": round(precision, 4), "recall": round(recall, 4),
                "f1": round(2 * precision * recall / max(1e-9, precision + recall), 4),
                "note": "阈值在 VALIDATION 上按精确率≥0.50 选定，TEST 只验不改"},
        },
    }
    report["calibration"] = [{"bin": str(row["bin"]), "n": int(row["n"]),
                              "predicted": round(float(row["predicted"]), 4),
                              "observed": round(float(row["observed"]), 4)}
                             for _, row in common.calibration_bins(y, prob, bins=10).iterrows()]

    served_mask = (test["y_waste"] == 0).to_numpy()
    served = test[served_mask].reset_index(drop=True)
    y_wait = served["wait_min"].to_numpy(dtype=float)
    wait_hat_all = np.clip(bundle["regressor"].predict(x_test), 0.0, None)
    wait_hat = wait_hat_all[served_mask]
    train_served = train[train["y_waste"] == 0]
    global_median = float(train_served["wait_min"].median())
    position_median = train_served.groupby("position_at_join")["wait_min"].median()
    maes = {
        "model": float(mean_absolute_error(y_wait, wait_hat)),
        "baselineStationHourMedian": float(mean_absolute_error(
            y_wait, predict_wait(bundle["waitBaselines"], served))),
        "baselinePositionMedian": float(mean_absolute_error(
            y_wait, served["position_at_join"].map(position_median).fillna(global_median))),
        "baselineGlobalMedian": float(mean_absolute_error(y_wait, np.full(len(y_wait), global_median))),
    }
    best_baseline = min(v for k, v in maes.items() if not k.startswith("model"))
    report["waitMinutes"] = {
        "evalSubset": "SERVED ∩ TEST（ABANDONED 的真实等待不可观测，属删失数据，不参与 MAE）",
        "rows": int(len(served)), "labelP50": round(float(np.median(y_wait)), 2),
        "labelP90": round(float(np.quantile(y_wait, 0.9)), 2),
        "modelMae": round(maes["model"], 4),
        "modelRmse": round(float(np.sqrt(np.mean((wait_hat - y_wait) ** 2))), 4),
        "baselineStationHourMedianMae": round(maes["baselineStationHourMedian"], 4),
        "baselinePositionMedianMae": round(maes["baselinePositionMedian"], 4),
        "baselineGlobalMedianMae": round(maes["baselineGlobalMedian"], 4),
        "bestBaselineName": "stationHourMedian" if maes["baselineStationHourMedian"] == best_baseline
        else ("positionMedian" if maes["baselinePositionMedian"] == best_baseline else "globalMedian"),
        "maeGainVsBestBaselinePct": round(100.0 * (1.0 - maes["model"] / best_baseline), 2),
        "p90AbsError": round(float(np.quantile(np.abs(wait_hat - y_wait), 0.9)), 3),
        "within2MinRate": round(float((np.abs(wait_hat - y_wait) <= 2.0).mean()), 4),
        "outputRangeLegal": bool(((wait_hat >= 0) & (wait_hat <= 120)).all()),
        "unit": "分钟",
    }

    report["ablation"] = ablation(frame, bundle, test, y)
    report["breakdown"] = build_breakdowns(test, prob, wait_hat_all)
    report["dataDiscipline"] = ("只读消费 clean 层 parquet；产物落 outputs/ml_queue/ 且全部独占创建；"
                                "特征截至 joined_at，并经随机抽样暴力复核 0 偏差（build_summary.leakAudit）")
    report["disclaimer"] = common.simulated_note()

    common.write_new_json(common.TEST_REPORT_PATH, report)
    common.write_new_bytes(common.TEST_REPORT_MD, markdown(report).encode("utf-8"))
    risk = report["abandonRisk"]
    point = risk["frozenOperatingPoint"]
    wait = report["waitMinutes"]
    print(f"[eval] TEST n={report['testRows']:,} 基础白排率={risk['baseRate']:.4f}")
    print(f"[eval] AUC={risk['auc']:.4f}（强基线 {risk['aucStrongBaseline']:.4f} / "
          f"排位基线 {risk['aucPositionOnlyBaseline']:.4f}）PR-AUC={risk['prAuc']:.4f} "
          f"Brier={risk['brier']:.4f} lift@10%={risk['liftAt10pct']:.2f}x "
          f"lift@20%={risk['liftAt20pct']:.2f}x")
    print(f"[eval] 阈值{point['threshold']:.3f} → 告警率{point['alertRate']:.2%} "
          f"精确率{point['precision']:.4f} 召回{point['recall']:.4f} F1={point['f1']:.4f}")
    print(f"[eval] 等待 MAE={wait['modelMae']:.3f}min（最优基线 {best_baseline:.3f} "
          f"[{wait['bestBaselineName']}]，降 {wait['maeGainVsBestBaselinePct']:.2f}%）"
          f" RMSE={wait['modelRmse']:.3f} 区间合法={wait['outputRangeLegal']}")
    print("[eval] 消融 AUC 仅排位={position:.4f} 仅静态={static:.4f} 仅动态={dynamic:.4f} 全量={full:.4f}"
          .format(position=report["ablation"]["positionOnly"], static=report["ablation"]["staticOnly"],
                  dynamic=report["ablation"]["dynamicsOnly"], full=report["ablation"]["full"]))
    print(f"[eval] -> {common.TEST_REPORT_MD}")
    return report


if __name__ == "__main__":
    main()
