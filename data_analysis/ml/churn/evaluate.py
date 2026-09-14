"""流失 TEST 首盲:冻结 AUC/PR-AUC/十分位提升表。选型只在 VALIDATION 完成。
用法(先完成 train):python -m data_analysis.ml.churn.evaluate
"""

from __future__ import annotations

import hashlib

import joblib
import numpy as np
import pandas as pd

from . import common
from .train import as_frame


def decile_table(y: np.ndarray, p: np.ndarray) -> list[dict]:
    order = np.argsort(-p)
    n = len(y)
    rows = []
    for d in range(10):
        lo, hi = n * d // 10, n * (d + 1) // 10
        idx = order[lo:hi]
        rows.append({"decile": d + 1, "n": int(len(idx)),
                     "churnCount": int(y[idx].sum()),
                     "churnRate": round(float(y[idx].mean()), 4)})
    return rows


def main() -> int:
    bundle = joblib.load(common.BUNDLE_PATH)
    model = bundle["model"]
    columns = bundle["feature_columns"]
    table = pd.read_pickle(common.USER_TABLE)
    test = table[table.split == "TEST"]
    y = test["churned_14d"].to_numpy()
    p = model.predict_proba(as_frame(test, columns))[:, 1]

    report = {
        "modelId": common.MODEL_ID, "modelVersion": common.MODEL_VERSION,
        "datasetId": common.DATASET_ID,
        "note": "流失预测 TEST 首盲;OBSERVE_END 与标签窗口见 bundle.metadata,重跑要求逐字节等价",
        # provenance 绑定:这组数字对应哪一版 bundle、哪一版特征表
        "bundleSha256": hashlib.sha256(common.BUNDLE_PATH.read_bytes()).hexdigest(),
        "userTableSha256": hashlib.sha256(common.USER_TABLE.read_bytes()).hexdigest(),
        "metricCaveats": "class_weight=balanced 使概率畸变,Brier 仅为畸变值;AUC/PR-AUC/lift 不受影响",
        "lineage": {"v1Quarantined": "v1(0.744 AUC)实锤两处越界:queues_90 泄入标签窗 + 标签窗超尾误标106人;"
                                      "v1 冻结件保留供审计但结论作废;v2 修口径后独立首盲",
                    "labelWindowFix": "标签窗 [OBSERVE_END, +14d) 双侧封闭;queues_90 加上界"},
        "testUsers": int(len(test)),
        "model": common.ranking_free_metrics(y, p),
        "recencyRule": common.ranking_free_metrics(
            y, test["days_since_last"].fillna(999).to_numpy()),
        "deciles": decile_table(y, p),
    }
    common.write_new_json(common.TEST_METRICS_PATH, report)
    for name in ("model", "recencyRule"):
        e = report[name]
        print(f"{name:12} auc={e['auc']:.4f} prAuc={e['prAuc']:.4f} brier={e['brier']} "
              f"lift@10%={e['liftAt10pct']} churnRate={e['churnRate']} n={e['n']}")
    print("decile churn rates:", [d["churnRate"] for d in report["deciles"]])
    print("saved (frozen) ->", common.TEST_METRICS_PATH)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
