"""join 好的训练帧落盘前跑两道 parity 审计:重建 vs 导出表、导出表 vs 原始小时表,任一最大差值 >1e-9 退出码 1。
导出特征是后续训练的唯一起点,lag 错位或 rolling 口径不一致会让指标虚高且难事后发现。
发布顺序(评审 P2#3):summary 无论如何都写(带 auditPassed 标志);审计通过才原子发布 joined_usable.pkl,
失败只留诊断、绝不让坏数据变成可训练缓存,也不覆盖此前有效的 pkl。下游用 common.require_prepared 把关。

用法(仓库根目录):
    python -m data_analysis.ml.load.prepare_data
"""

from __future__ import annotations

import json
import os
import sys

from . import common


def main() -> int:
    manifest = common.read_manifest()
    frame = common.load_training_frame()

    lag_cols = common.LAG_FEATURES
    missing_lags = frame[lag_cols].isna().any(axis=1)
    usable = frame[~missing_lags].copy()

    audit = common.audit_offline_parity(usable)
    worst = max(audit.values())
    # parity 的 rolling 就是从 lag 列重算,lag 块整块平移它检不出;
    # 补一道:按 reference_dt - k 小时回连原始小时表直接核对。
    raw_audit = common.audit_raw_alignment(usable, common.load_hourly_metrics())
    worst = max(worst, raw_audit["worstAbsDiff"], float(raw_audit["missingRawHistoryHours"]))
    calendar = common.build_calendar_lookup(usable[usable["split_1h"] != "EXCLUDED"])

    summary = {
        "datasetId": manifest["datasetId"],
        "publishedBatchId": manifest["publishedBatchId"],
        "pipelineRunId": manifest["pipelineRunId"],
        "featureVersion": frame["feature_version"].unique().tolist(),
        "mlSplits": manifest["mlSplits"],
        "joinedRows": int(len(frame)),
        "rowsWithMissingLags": int(missing_lags.sum()),
        "usableRows": int(len(usable)),
        "splitCounts": {
            column: frame[column].value_counts().to_dict()
            for column in common.HORIZON_SPLITS.values()
        },
        "offlineRebuildMaxAbsDiff": audit,
        "rawHourlyAlignment": raw_audit,
    }

    summary["auditPassed"] = bool(worst <= 1e-9)
    out = common.DATA_ANALYSIS_ROOT / "outputs" / "ml_load"
    publish(usable, summary, out)
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    if not summary["auditPassed"]:
        print("PARITY AUDIT FAILED; usable cache NOT published", file=sys.stderr)
        return 1
    print("offline rebuild matches exported features; frame saved to", out / "joined_usable.pkl")
    return 0


def publish(usable: "pd.DataFrame", summary: dict, out: "common.Path") -> None:
    """审计通过才发布可用缓存(评审 P2#3);独立成函数供回归测试直接驱动。
    summary 无条件写(tmp+replace 防半截 JSON),auditPassed=False 会让 require_prepared 拦下游。"""
    out.mkdir(parents=True, exist_ok=True)
    tmp_summary = out / "prepare_summary.json.tmp"
    with open(tmp_summary, "w", encoding="utf-8") as handle:
        json.dump(summary, handle, ensure_ascii=False, indent=2)
    os.replace(tmp_summary, out / "prepare_summary.json")
    if not summary["auditPassed"]:
        return
    tmp_pkl = out / "joined_usable.pkl.tmp"
    usable.to_pickle(tmp_pkl)
    os.replace(tmp_pkl, out / "joined_usable.pkl")


if __name__ == "__main__":
    raise SystemExit(main())
