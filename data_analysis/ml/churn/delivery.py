"""Fixed-snapshot user risk ranking; scores are not calibrated probabilities."""
from __future__ import annotations

import numpy as np
import pandas as pd
from sklearn.ensemble import HistGradientBoostingClassifier

from . import common

MODEL_ID = "hgb-churn-asof-v3"


def frame(table, categories):
    out = table[common.FEATURE_COLUMNS].copy()
    for column, levels in categories.items():
        out[column] = pd.Categorical(out[column].astype(str), categories=levels)
    return out


def score(bundle, table):
    return bundle["model"].predict_proba(frame(table, bundle["categories"]))[:, 1]


def fit(table):
    train = table[table.split == "TRAIN"]
    valid = table[table.split == "VALIDATION"]
    test = table[table.split == "TEST"]
    if any(part.empty or part.churned_14d.nunique() != 2 for part in (train, valid, test)):
        raise ValueError("Churn needs both classes in each nonempty user-disjoint split")
    categories = {column: sorted(train[column].astype(str).unique()) for column in common.CATEGORICAL}
    # Unweighted: remove the legacy class-weight distortion. Still label output
    # as a ranking score; calibration / intervention benefit are not established.
    model = HistGradientBoostingClassifier(
        categorical_features="from_dtype", max_iter=180, learning_rate=.06,
        max_leaf_nodes=31, min_samples_leaf=30, l2_regularization=1.0,
        early_stopping=False, random_state=42)
    model.fit(frame(train, categories), train.churned_14d)
    bundle = {"model": model, "categories": categories, "modelId": MODEL_ID}
    train_scores = score(bundle, train)
    bundle["riskThresholds"] = [float(np.quantile(train_scores, .7)), float(np.quantile(train_scores, .9))]
    metrics = {}
    for name, part in (("validation", valid), ("test", test)):
        y = part.churned_14d.to_numpy()
        predicted = score(bundle, part)
        metrics[name] = {
            "model": common.ranking_free_metrics(y, predicted),
            "recencyBaseline": common.ranking_free_metrics(y, part.days_since_last.fillna(999).to_numpy()),
            "constantBaseline": common.ranking_free_metrics(y, np.full(len(y), train.churned_14d.mean())),
        }
    metrics["protocol"] = {
        "target": "No charging attempt in next 14 days, not permanent churn",
        "referenceTime": common.OBSERVE_END.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "businessReferenceTime": "2026-05-15T00:00:00+08:00",
        "labelEndExclusive": (common.OBSERVE_END + pd.Timedelta(days=14)).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "split": "User-disjoint sha1(42:user_id) buckets 70/15/15; single historical snapshot, not temporal deployment validation",
        "featurePolicy": "Fixed observation windows; completed session money in CNY yuan; only resolved queue outcomes; no future registrations",
        "scoreSemantics": "Uncalibrated risk-ranking score in [0,1], not a promised probability or coupon intervention benefit",
        "riskBands": "TRAIN score p70/p90 fixed thresholds, not clinical/business certainty",
        "trainUsers": len(train), "validationUsers": len(valid), "testUsers": len(test),
    }
    return bundle, metrics


def describe(bundle, row):
    p = float(score(bundle, row.to_frame().T)[0])
    low, high = bundle["riskThresholds"]
    age = None if pd.isna(row.days_since_last) else round(float(row.days_since_last), 1)
    count = int(row.attempts_30)
    energy = round(float(row.energy_kwh_90), 2)
    spend = round(float(row.spend_yuan_90), 2)
    facts = [f"过去30天有 {count} 次充电请求", f"过去90天已结束会话共 {energy:g} kWh，费用 {spend:.2f} 元"]
    facts.insert(0, "观察日前没有充电请求记录" if age is None else f"距最近一次充电请求 {age:g} 天")
    return {
        "userId": str(row.user_id), "cityId": str(row.home_city_id),
        "riskScore": round(p, 6), "riskLevel": "HIGH" if p >= high else "MEDIUM" if p >= low else "LOW",
        "scoreSemantics": "RISK_RANKING_NOT_CALIBRATED_PROBABILITY", "evaluationSplit": str(row.split),
        "referenceTime": common.OBSERVE_END.strftime("%Y-%m-%dT%H:%M:%SZ"), "modelId": MODEL_ID,
        "features": {"daysSinceLastAttempt": age, "attempts30d": count,
                     "energyKwh90d": energy, "feesYuan90d": spend, "queueJoins90d": int(row.queues_90)},
        "explanations": facts, "explanationSemantics": "Observed feature facts, not causal attribution",
        "dataKind": "SIMULATED",
    }
