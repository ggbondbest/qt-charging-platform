"""Reproducible CPU training and one chronological held-out evaluation.

Usage: python -m data_analysis.chargepilot.ml.train --output data_analysis/outputs/chargepilot
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import time

os.environ.setdefault("OMP_NUM_THREADS", "2")
os.environ.setdefault("OPENBLAS_NUM_THREADS", "2")
import joblib
import numpy as np
import sklearn
from sklearn.ensemble import HistGradientBoostingClassifier, HistGradientBoostingRegressor
from sklearn.metrics import mean_absolute_error, mean_squared_error, log_loss, roc_auc_score
from threadpoolctl import threadpool_limits

from .data import DATASET, FEATURE_NAMES, HORIZONS, iso, prepare, purged_mask, split_boundaries


def temperature(probabilities, value):
    z = np.log(np.clip(probabilities, 1e-8, 1)) / value
    z -= z.max(axis=1, keepdims=True)
    p = np.exp(z)
    return p / p.sum(axis=1, keepdims=True)


def choose_temperature(model, x, y):
    p = model.predict_proba(x)
    candidates = [0.7, 0.85, 1.0, 1.15, 1.35, 1.7]
    scores = [log_loss(y, temperature(p, t), labels=model.classes_) for t in candidates]
    return float(candidates[int(np.argmin(scores))])


def availability_samples(replay, bounds, limit, rng):
    start, end = bounds
    # Draw unique station/origin pairs, then a horizon. Purge incomplete target intervals.
    first = (start - replay.start) // 300
    n_ticks = (end - start) // 300
    total = len(replay.catalog) * n_ticks
    indices = rng.choice(total, size=min(total, int(limit * 1.15) + 100), replace=False)
    s = indices // n_ticks
    ref = replay.start + (first + indices % n_ticks) * 300
    h = rng.choice(HORIZONS, size=len(s))
    target = ref + h * 60
    mask = purged_mask(ref, target, target + 299, start, end)
    s, ref, h, target = (v[mask][:limit] for v in [s, ref, h, target])
    y = replay.counts[s, (target - replay.start) // 300, 0]
    return {"x": replay.features(s, ref, h), "y": y, "s": s, "ref": ref, "h": h,
            "arrival": target, "final": target + 299, "eligibleOrigins": total,
            "purgedDraws": int((~mask).sum())}


def wait_samples(replay, labels, bounds, limit, rng):
    start, end = bounds
    # Each factual attempt contributes at most three distinct lead times. The target outcome
    # never enters features; all replicas stay in the same chronological split.
    label = labels[(labels.arrivalEpoch >= start) & (labels.arrivalEpoch < end)].copy()
    indices = rng.choice(len(label) * len(HORIZONS), size=min(len(label) * 3, len(label) * len(HORIZONS)), replace=False)
    row = label.iloc[indices // len(HORIZONS)]
    h = HORIZONS[indices % len(HORIZONS)]
    arrival = row.arrivalEpoch.to_numpy(dtype=np.int64)
    ref = arrival - h * 60
    final = row.finalEpoch.to_numpy(dtype=np.int64)
    mask = purged_mask(ref, arrival, final, start, end)
    selected = np.flatnonzero(mask)[:limit]
    s = row.stationCode.to_numpy(dtype=int)[selected]
    return {"x": replay.features(s, ref[selected], h[selected]),
            "success": row.success.to_numpy(dtype=int)[selected],
            "wait": row.waitMinutes.to_numpy(dtype=float)[selected],
            "s": s, "ref": ref[selected], "h": h[selected], "arrival": arrival[selected],
            "final": final[selected], "uniqueAttempts": int(row.iloc[selected].attempt_id.nunique()),
            "purgedDraws": int((~mask).sum())}


def mean_metrics(y, pred):
    return {"maeMinutes": float(mean_absolute_error(y, pred)),
            "rmseMinutes": float(np.sqrt(mean_squared_error(y, pred)))}


def availability_metrics(y, p):
    p = p / p.sum(axis=1, keepdims=True)
    classes = np.arange(p.shape[1])
    expected = p @ classes
    actual_available = y > 0
    available = 1 - p[:, 0]
    ece = 0.0
    for lo in np.arange(0, 1, .1):
        m = (available >= lo) & (available <= 1 if lo > .89 else available < lo + .1)
        if m.any():
            ece += float(m.mean() * abs(actual_available[m].mean() - available[m].mean()))
    return {"n": len(y), "countMae": float(mean_absolute_error(y, expected)),
            "countRmse": float(np.sqrt(mean_squared_error(y, expected))),
            "brier": float(np.mean((available - actual_available) ** 2)),
            "logLoss": float(log_loss(y, p, labels=classes)), "ece10": ece,
            "accuracy": float(np.mean(np.argmax(p, axis=1) == y)),
            "observedAvailableRate": float(actual_available.mean()),
            "predictedAvailableRate": float(available.mean())}


def group_keys(sample):
    # Local arrival hour is known at recommendation time; all mean tables fit TRAIN only.
    hour = ((sample["arrival"] + 8 * 3600) // 3600) % 24
    return sample["s"] * 24 + hour


def run(output, dataset=DATASET, max_train=160_000, max_evaluation=35_000, iterations=140, seed=42, threads=2):
    started = time.monotonic()
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    print("Preparing complete CLEAN five-minute histories...", flush=True)
    replay, labels, source = prepare(dataset)
    rng = np.random.default_rng(seed)
    bounds = split_boundaries(replay.config)
    samples = {name: availability_samples(replay, window, max_train if name == "TRAIN" else max_evaluation, rng)
               for name, window in bounds.items()}
    waits = {name: wait_samples(replay, labels, window, max_train if name == "TRAIN" else max_evaluation, rng)
             for name, window in bounds.items()}
    params = {"max_iter": iterations, "learning_rate": .08, "max_leaf_nodes": 31,
              "min_samples_leaf": 30, "l2_regularization": 2.0,
              "early_stopping": False, "random_state": seed,
              "categorical_features": [0, 1, 2]}
    train, val = samples["TRAIN"], samples["VALIDATION"]
    wt, wv = waits["TRAIN"], waits["VALIDATION"]
    valid = wt["success"] == 1
    fit_started = time.monotonic()
    with threadpool_limits(limits=threads):
        print(f"Fitting availability on {len(train['y'])} rows...", flush=True)
        availability = HistGradientBoostingClassifier(**params).fit(train["x"], train["y"])
        print(f"Fitting success / conditional mean / P90 on {len(wt['success'])} rows...", flush=True)
        success = HistGradientBoostingClassifier(**params).fit(wt["x"], wt["success"])
        mean = HistGradientBoostingRegressor(loss="squared_error", **params).fit(wt["x"][valid], wt["wait"][valid])
        q90 = HistGradientBoostingRegressor(loss="quantile", quantile=.9, **params).fit(wt["x"][valid], wt["wait"][valid])
        calibration = {"availabilityTemperature": choose_temperature(availability, val["x"], val["y"]),
                       "successTemperature": choose_temperature(success, wv["x"], wv["success"])}
        fit_seconds = time.monotonic() - fit_started
        print("Evaluating frozen models on chronological validation/test...", flush=True)
        n_classes = int(replay.capacity.max()) + 1
        if not np.array_equal(availability.classes_, np.arange(n_classes)):
            raise ValueError("Training did not cover every available-count class")
        group_count = len(replay.catalog) * 24
        climatology = np.ones((group_count, n_classes), dtype=float)
        np.add.at(climatology, (group_keys(train), train["y"]), 1)
        climatology /= climatology.sum(axis=1, keepdims=True)
        wk = group_keys(wt)[valid]
        wait_sum = np.bincount(wk, weights=wt["wait"][valid], minlength=group_count)
        wait_count = np.bincount(wk, minlength=group_count)
        wait_climate = (wait_sum + wt["wait"][valid].mean() * 5) / (wait_count + 5)
        metrics = {}
        for split in ["VALIDATION", "TEST"]:
            a, w = samples[split], waits[split]
            p = temperature(availability.predict_proba(a["x"]), calibration["availabilityTemperature"])
            persistence = np.eye(n_classes)[a["x"][:, FEATURE_NAMES.index("last_available")].astype(int)]
            avail = {"model": availability_metrics(a["y"], p),
                     "persistence": availability_metrics(a["y"], np.clip(persistence, 1e-8, 1)),
                     "stationHour": availability_metrics(a["y"], climatology[group_keys(a)]),
                     "byHorizon": {str(int(h)): availability_metrics(a["y"][a["h"] == h], p[a["h"] == h])
                                   for h in HORIZONS if (a["h"] == h).any()}}
            good = w["success"] == 1
            y = w["wait"][good]
            predicted = np.maximum(0, mean.predict(w["x"][good]))
            high = np.maximum(predicted, np.maximum(0, q90.predict(w["x"][good])))
            sp = temperature(success.predict_proba(w["x"]), calibration["successTemperature"])[:, 1]
            wait = {"n": len(y), "uniqueServiceAttempts": w["uniqueAttempts"],
                    "condition": "Successful non-reservation charging attempts only; failures have unknown wait",
                    "model": mean_metrics(y, predicted), "zeroWait": mean_metrics(y, np.zeros_like(y)),
                    "stationHour": mean_metrics(y, wait_climate[group_keys(w)[good]]),
                    "nonzeroWaitMaeMinutes": float(mean_absolute_error(y[y > 0], predicted[y > 0])),
                    "p90Coverage": float(np.mean(y <= high)),
                    "p90PinballLoss": float(np.mean(np.maximum(.9 * (y - high), -.1 * (y - high)))),
                    "observedMeanMinutes": float(y.mean()), "predictedMeanMinutes": float(predicted.mean())}
            service = {"n": len(sp), "brier": float(np.mean((sp - w["success"]) ** 2)),
                       "logLoss": float(log_loss(w["success"], sp)),
                       "rocAuc": float(roc_auc_score(w["success"], sp)),
                       "constantTrainRateBrier": float(np.mean((wt["success"].mean() - w["success"]) ** 2)),
                       "observedSuccessRate": float(w["success"].mean())}
            metrics[split.lower()] = {"availability": avail, "wait": wait, "service": service}
    bundle = {"availability": availability, "success": success, "waitMean": mean, "waitP90": q90,
              "calibration": calibration, "featureNames": FEATURE_NAMES, "modelVersion": "1.0.0",
              "provenance": replay.config["provenance"]}
    joblib.dump(bundle, output / "arrival.joblib", compress=3)
    replay.save(output)
    metadata = {"modelId": "chargepilot-arrival-hgb-v1", "modelVersion": "1.0.0",
                "dataSource": "SIMULATED", "resolutionMinutes": 5, "horizonsMinutes": HORIZONS.tolist(),
                "featureNames": FEATURE_NAMES, "featureCount": len(FEATURE_NAMES),
                "asOfRule": "Only telemetry intervals ending at or before origin; final same-tick state excluded",
                "waitDefinition": "started_at - attempted_at, conditioned on successful non-reservation charging",
                "splitBoundaries": {k: {"start": iso(v[0]), "endExclusive": iso(v[1])} for k, v in bounds.items()},
                "samples": {k: {"availability": len(samples[k]["y"]), "waitSuccess": len(waits[k]["success"]),
                                 "conditionalWait": int(waits[k]["success"].sum()),
                                 "uniqueAttempts": waits[k]["uniqueAttempts"],
                                 "availabilityPurgedDraws": samples[k]["purgedDraws"],
                                 "waitPurgedDraws": waits[k]["purgedDraws"],
                                 "maxLabelTime": iso(max(samples[k]["final"].max(), waits[k]["final"].max()))}
                            for k in bounds},
                "parameters": params, "seed": seed, "threads": threads,
                "calibration": {**calibration, "fitSplit": "VALIDATION", "method": "scalar temperature minimizing log loss"},
                "metrics": metrics, "source": source, "provenance": replay.config["provenance"], "fitSeconds": fit_seconds,
                "totalSeconds": time.monotonic() - started,
                "versions": {"python": platform.python_version(), "sklearn": sklearn.__version__, "numpy": np.__version__},
                "limitations": ["Simulated data, not validated real-world accuracy", "Five-minute state intervals",
                                "Wait estimates are conditional on obtaining service; failures are not zero-wait labels",
                                "Bounded deterministic station-time samples; lead-time replicas are not independent users",
                                "ETA is supplied externally, not learned from route observations",
                                "P90 was validated globally, not a guarantee for an individual trip"],
                "artifacts": {name: {"sha256": hashlib.sha256((output / name).read_bytes()).hexdigest(),
                                      "bytes": (output / name).stat().st_size}
                              for name in ["arrival.joblib", "replay.npz", "replay.json"]}}
    (output / "arrival.metadata.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2, allow_nan=False))
    print(json.dumps({"output": str(output), "fitSeconds": fit_seconds, "totalSeconds": metadata["totalSeconds"],
                      "test": metrics["test"]}, indent=2), flush=True)
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", default="data_analysis/outputs/chargepilot")
    parser.add_argument("--dataset", default=str(DATASET))
    parser.add_argument("--max-train", type=int, default=160_000)
    parser.add_argument("--max-evaluation", type=int, default=35_000)
    parser.add_argument("--iterations", type=int, default=140)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--threads", type=int, default=2)
    args = parser.parse_args()
    if min(args.max_train, args.max_evaluation, args.iterations, args.threads) <= 0:
        parser.error("Sample limits, iterations, and threads must be positive")
    run(args.output, args.dataset, args.max_train, args.max_evaluation, args.iterations, args.seed, args.threads)


if __name__ == "__main__":
    main()
