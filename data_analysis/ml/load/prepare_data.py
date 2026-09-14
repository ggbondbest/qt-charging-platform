"""join 好的训练帧落盘前跑两道 parity 审计:重建 vs 导出表、导出表 vs 原始小时表,任一最大差值 >1e-9 退出码 1。
导出特征是后续训练的唯一起点,lag 错位或 rolling 口径不一致会让指标虚高且难事后发现。

用法(仓库根目录):
    python -m data_analysis.ml.load.prepare_data
"""

from __future__ import annotations

import json
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

    out = common.DATA_ANALYSIS_ROOT / "outputs" / "ml_load"
    out.mkdir(parents=True, exist_ok=True)
    usable.to_pickle(out / "joined_usable.pkl")
    with open(out / "prepare_summary.json", "w", encoding="utf-8") as handle:
        json.dump(summary, handle, ensure_ascii=False, indent=2)
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    if worst > 1e-9:
        print("PARITY AUDIT FAILED", file=sys.stderr)
        return 1
    print("offline rebuild matches exported features; frame saved to", out / "joined_usable.pkl")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
