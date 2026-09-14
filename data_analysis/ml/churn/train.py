"""流失训练:HistGB 二分类(class_weight=balanced)学 P(14天内不再来);
VALIDATION 上报 AUC/PR-AUC/Brier/top-decile 提升,基线是纯 recency 规则与随机;TEST 一行不看。
用法(仓库根目录):python -m data_analysis.ml.churn.train
"""

from __future__ import annotations

import json
import platform
import time

import joblib
import numpy as np
import pandas as pd
import sklearn
from sklearn.ensemble import HistGradientBoostingClassifier

from . import common

PARAMS = {
    "max_iter": 300,
    "learning_rate": 0.05,
    "max_leaf_nodes": 31,
    "min_samples_leaf": 30,
    "l2_regularization": 1.0,
    "early_stopping": False,
    "random_state": common.SEED,
    "class_weight": "balanced",
}


def feature_columns(table: pd.DataFrame) -> list[str]:
    return [c for c in table.columns if c not in common.NON_FEATURE]


def as_frame(table: pd.DataFrame, columns: list[str]) -> pd.DataFrame:
    frame = table[columns].copy()
    for column in common.CATEGORICAL:
        frame[column] = pd.Categorical(frame[column].astype(str))
    return frame


def main() -> int:
    started = time.time()
    common.OUT_DIR.mkdir(parents=True, exist_ok=True)
    if common.USER_TABLE.exists():
        table = pd.read_pickle(common.USER_TABLE)
    else:
        table = common.build_user_table()
        table.to_pickle(common.USER_TABLE)
    columns = feature_columns(table)
    train = table[table.split == "TRAIN"]
    valid = table[table.split == "VALIDATION"]

    model = HistGradientBoostingClassifier(categorical_features="from_dtype", **PARAMS)
    model.fit(as_frame(train, columns), train["churned_14d"].astype(int))
    p_valid = model.predict_proba(as_frame(valid, columns))[:, 1]

    report = {
        "model": common.ranking_free_metrics(valid["churned_14d"].to_numpy(), p_valid),
        "recencyRule": common.ranking_free_metrics(
            valid["churned_14d"].to_numpy(), valid["days_since_last"].fillna(999).to_numpy()),
    }
    for name, entry in report.items():
        print(f"{name:14} auc={entry['auc']:.4f} prAuc={entry['prAuc']:.4f} "
              f"lift@10%={entry['liftAt10pct']} n={entry['n']} churnRate={entry['churnRate']}")

    with open(common.TRAIN_METRICS_PATH, "w", encoding="utf-8") as handle:
        json.dump({"validation": report, "featureColumns": columns,
                   "seconds": round(time.time() - started, 1)}, handle,
                  ensure_ascii=False, indent=2)
    bundle = {
        "model": model, "feature_columns": columns,
        "model_id": common.MODEL_ID, "model_version": common.MODEL_VERSION,
        "metadata": {
            "datasetId": common.DATASET_ID,
            "task": "user churn in next 14 days (classification)",
            "observeEnd": str(common.OBSERVE_END.date()),
            "labelHorizonDays": common.LABEL_HORIZON_DAYS,
            "params": PARAMS,
            "validationMetrics": report,
            "splitRule": "sha1(SEED:user_id)%100: <70 TRAIN, <85 VALIDATION, else TEST",
            "dependencies": {"python": platform.python_version(),
                             "scikit-learn": sklearn.__version__,
                             "pandas": pd.__version__, "numpy": np.__version__},
            "trainedAt": pd.Timestamp.utcnow().isoformat(),
        },
    }
    joblib.dump(bundle, common.BUNDLE_PATH)
    print("bundle ->", common.BUNDLE_PATH, f"({round(time.time() - started, 1)}s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
