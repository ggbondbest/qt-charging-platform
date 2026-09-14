"""流失 TEST 首盲:冻结 AUC/PR-AUC/十分位提升表。选型只在 VALIDATION 完成。
用法(先完成 train):python -m data_analysis.ml.churn.evaluate
"""

from __future__ import annotations

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
