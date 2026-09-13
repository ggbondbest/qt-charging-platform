"""B1 lean feature-pruned candidate sweep (VALIDATION selection only).

Prunes the 39-column shipped ``hgb-deep`` feature set using the permutation
importance ranking produced by ``python -m data_analysis.ml.load.feature_importance``
(VALIDATION-only), refits the winner recipe (per-horizon
HistGradientBoostingRegressor, max_iter=600 / lr=0.03 / 63 leaves / min_samples_leaf=20
/ l2=0.5 / no early stopping / seed 42, ``categorical_features="from_dtype"``)
for three candidate subsets plus a full-39 control, and scores every contract
horizon on VALIDATION with [0, rated_capacity_kw] clipping under TWO slice
conventions: (a) "contract" -- each horizon under its own split column
(h01->split_1h, h06->split_6h, h24->split_24h), as prescribed by this task's
procedure, and (b) "split24h" -- all horizons on the split_24h VALIDATION
window, which is the convention the shipped 13.2938 baseline and the race
ablation numbers were actually computed under (train.py). The recomputed
full-39 control makes both comparisons apples-to-apples on identical slices.

Selection rule: lowest VALIDATION mean MAE over the three contract horizons
among L1/L2/L3 under the prescribed "contract" convention (the full-39 control
is reference only; split24h-convention numbers are reported alongside, and a
disagreement between the two would be surfaced). The winning subset is
saved as ``race2/hgb-lean-v1-candidate.joblib``, a bundle dict identical in
shape to the shipped one but with ``feature_columns`` = the subset, metadata
adapted (featureVersion "history24-lean-v1", dropped features recorded). A
final parity check rebuilds the subset matrix for 5 random VALIDATION windows
from raw hourly history via ``common.build_feature_row`` (the online code
path) and verifies exact agreement with the offline table values.

HARD RULES honored here: models fit only on ``split_24h == "TRAIN"`` rows with
a non-null finite label; all comparisons use VALIDATION only; TEST rows are
never read, scored, or touched.

Usage (repo root):
    KMP_DUPLICATE_LIB_OK=TRUE python -m data_analysis.ml.load.train_lean
"""

from __future__ import annotations

import hashlib
import json
import pickle
import platform
import time
from datetime import datetime, timezone

import joblib
import numpy as np
import pandas as pd
import sklearn
from sklearn.ensemble import HistGradientBoostingRegressor

from . import common

SEED = 42
PARAMS = {
    "max_iter": 600,
    "learning_rate": 0.03,
    "max_leaf_nodes": 63,
    "min_samples_leaf": 20,
    "l2_regularization": 0.5,
    "early_stopping": False,
    "random_state": SEED,
}
OUT_DIR = common.DATA_ANALYSIS_ROOT / "outputs" / "ml_load"
RACE2_DIR = OUT_DIR / "race2"
IMPORTANCE_REPORT_PATH = OUT_DIR / "analysis" / "feature_importance.json"
BUNDLE_PATH = RACE2_DIR / "hgb-lean-v1-candidate.joblib"
METADATA_PATH = RACE2_DIR / "hgb-lean-v1-candidate.metadata.json"
RESULTS_PATH = RACE2_DIR / "train_lean_results.json"

CANDIDATE_MODEL_ID = "hgb-lean-v1-candidate"
CANDIDATE_MODEL_VERSION = "0.3.0"
CANDIDATE_FEATURE_VERSION = "history24-lean-v1"
CONTRACT_HORIZONS = (1, 6, 24)
BASELINE_TO_BEAT = 13.2938

# L1/L2 are defined as "drop the N lowest-importance features"; L3 is the
# literal recent-lags-style spec from the task brief (lags h01-h06, the three
# 24h-window rolling stats, all calendar columns, and the city/station/
# capacity columns including last_available_count which is a station column).
L1_DROP_COUNT = 15
L2_DROP_COUNT = 20
L3_FEATURES = [
    "city_id",
    "station_id",
    "hour_of_day",
    "day_of_week",
    "is_weekend",
    "is_public_holiday",
    "is_adjusted_workday",
    "capacity",
    "rated_capacity_kw",
    "last_available_count",
    *[f"lag_power_kw_h{i:02d}" for i in range(1, 7)],
    "rolling_mean_kw_24h",
    "rolling_std_kw_24h",
    "rolling_max_kw_24h",
]


def canonical_order(columns: list[str]) -> list[str]:
    """Subset columns re-ordered to the canonical FEATURE_COLUMNS order."""
    return [c for c in common.FEATURE_COLUMNS if c in set(columns)]


def load_ranking() -> list[str]:
    """Feature names sorted by importance rank (1 first) from the committed
    permutation-importance report."""
    with open(IMPORTANCE_REPORT_PATH, encoding="utf-8") as handle:
        report = json.load(handle)
    table = sorted(report["ranking"], key=lambda row: row["rank"])
    ranked = [row["feature"] for row in table]
    if sorted(ranked) != sorted(common.FEATURE_COLUMNS):
        raise AssertionError("importance report does not cover exactly the 39 features")
    return ranked


def candidate_subsets() -> dict[str, list[str]]:
    ranked = load_ranking()
    l1 = canonical_order(ranked[: len(ranked) - L1_DROP_COUNT])
    l2 = canonical_order(ranked[: len(ranked) - L2_DROP_COUNT])
    l3 = canonical_order(L3_FEATURES)
    for name, subset in (("L1", l1), ("L2", l2), ("L3", l3)):
        if "station_id" not in subset or "hour_of_day" not in subset:
            raise AssertionError(f"{name} illegally dropped station_id or hour_of_day")
    return {
        "full39": list(common.FEATURE_COLUMNS),
        "L1": l1,
        "L2": l2,
        "L3": l3,
    }


def clip_to_capacity(values: np.ndarray, frame: pd.DataFrame, index) -> np.ndarray:
    limits = frame.loc[index, "rated_capacity_kw"].to_numpy(dtype=float)
    return np.clip(values, 0.0, limits)


def evaluate_subset(
    name: str,
    subset: list[str],
    frame: pd.DataFrame,
    full_matrix: pd.DataFrame,
) -> dict:
    """Fit 24 winner-recipe models on TRAIN and score the contract horizons
    on VALIDATION under each horizon's own split column, clipped."""
    matrix = full_matrix[subset]
    mask_train = frame["split_24h"] == "TRAIN"
    per_horizon: dict[str, dict] = {}
    models: dict[int, HistGradientBoostingRegressor] = {}
    started = time.time()
    for horizon in range(1, 25):
        label_col = f"label_power_kw_h{horizon:02d}"
        label = frame[label_col]
        train_ok = mask_train & label.notna() & np.isfinite(label)
        model = HistGradientBoostingRegressor(categorical_features="from_dtype", **PARAMS)
        model.fit(matrix.loc[train_ok], frame.loc[train_ok, label_col].to_numpy(dtype=float))
        models[horizon] = model

        if horizon in CONTRACT_HORIZONS:
            # Two VALIDATION conventions, same model, same clipping:
            #  * "contract": each horizon under its own split column
            #    (h01->split_1h, h06->split_6h, h24->split_24h) -- the
            #    convention this task's procedure prescribes.
            #  * "split24h": all horizons on the split_24h VALIDATION window
            #    -- the convention the shipped 13.2938 baseline and the race
            #    ablation numbers were computed under, recomputed here so the
            #    comparison is apples-to-apples on identical slices.
            for convention, split_col in (
                ("contract", common.HORIZON_SPLITS[horizon]),
                ("split24h", "split_24h"),
            ):
                valid_ok = (
                    (frame[split_col] == "VALIDATION") & label.notna() & np.isfinite(label)
                )
                valid_index = frame.index[valid_ok]
                y_valid = frame.loc[valid_ok, label_col].to_numpy(dtype=float)
                y_pred = clip_to_capacity(
                    model.predict(matrix.loc[valid_ok]).astype(float), frame, valid_index
                )
                error = y_pred - y_valid
                entry = per_horizon.setdefault(f"h{horizon:02d}", {})
                entry[convention] = {
                    "mae": float(np.abs(error).mean()),
                    "rmse": float(np.sqrt((error**2).mean())),
                    "n": int(len(y_valid)),
                    "splitColumn": split_col,
                    "clippedToRatedCapacity": True,
                }
            print(
                f"[{name}] h{horizon:02d} fitted "
                f"contract_val_mae={per_horizon[f'h{horizon:02d}']['contract']['mae']:.4f} kW "
                f"(n={per_horizon[f'h{horizon:02d}']['contract']['n']})  "
                f"split24h_val_mae={per_horizon[f'h{horizon:02d}']['split24h']['mae']:.4f} kW"
            )
        else:
            print(f"[{name}] h{horizon:02d} fitted")
    maes = {
        convention: [
            per_horizon[f"h{h:02d}"][convention]["mae"] for h in CONTRACT_HORIZONS
        ]
        for convention in ("contract", "split24h")
    }
    rmses = {
        convention: [
            per_horizon[f"h{h:02d}"][convention]["rmse"] for h in CONTRACT_HORIZONS
        ]
        for convention in ("contract", "split24h")
    }
    return {
        "subset": subset,
        "nFeatures": len(subset),
        "models": models,
        "validation": per_horizon,
        "meanValMaeContract": float(np.mean(maes["contract"])),
        "meanValMaeSplit24h": float(np.mean(maes["split24h"])),
        "pooledRmseContract": float(np.sqrt(np.mean(np.square(rmses["contract"])))),
        "pooledRmseSplit24h": float(np.sqrt(np.mean(np.square(rmses["split24h"])))),
        "contractRowsContract": sum(
            per_horizon[f"h{h:02d}"]["contract"]["n"] for h in CONTRACT_HORIZONS
        ),
        "contractRowsSplit24h": sum(
            per_horizon[f"h{h:02d}"]["split24h"]["n"] for h in CONTRACT_HORIZONS
        ),
        "seconds": round(time.time() - started, 1),
    }


def parity_check(
    frame: pd.DataFrame,
    hourly: pd.DataFrame,
    subset: list[str],
    calendar: dict,
) -> dict:
    """Rebuild 5 random VALIDATION windows through the online code path and
    verify the subset columns agree with the offline feature table exactly."""
    pool = frame[frame["split_24h"] == "VALIDATION"]
    hourly_by_station = {key: group for key, group in hourly.groupby("station_id")}
    candidates = pool.sample(n=40, random_state=13)
    checked = 0
    max_numeric_diff = 0.0
    categorical_mismatches = 0
    per_row: list[dict] = []
    for row in candidates.itertuples():
        station_hours = hourly_by_station.get(row.station_id)
        if station_hours is None:
            continue
        window = station_hours[
            (station_hours["recorded_at"] >= row.reference_dt - pd.Timedelta(hours=24))
            & (station_hours["recorded_at"] < row.reference_dt)
        ]
        if len(window) != 24:
            continue
        history = [
            {
                "station_id": rec.station_id,
                "city_id": rec.city_id,
                "recorded_at": common.format_utc(rec.recorded_at.to_pydatetime()),
                "mean_power_kw": float(rec.mean_power_kw),
                "capacity": int(rec.capacity),
                # rated_capacity_kw is a station attribute, taken from the
                # joined frame row exactly like predict.py's smoke test.
                "rated_capacity_kw": float(row.rated_capacity_kw),
                "end_available_count": int(rec.end_available_count),
            }
            for rec in window.itertuples(index=False)
        ]
        built = common.build_feature_row(history, row.reference_time, calendar)
        offline = common.features_matrix(frame.loc[[row.Index]])[subset]
        online = pd.DataFrame([built], columns=subset)
        for column in common.CATEGORICAL_FEATURES:
            if column in subset:
                online[column] = online[column].astype("category")
        row_diffs: dict[str, float] = {}
        for column in subset:
            if column in common.CATEGORICAL_FEATURES:
                same = str(online[column].iloc[0]) == str(offline[column].iloc[0])
                if not same:
                    categorical_mismatches += 1
                row_diffs[column] = 0.0 if same else float("inf")
            else:
                diff = abs(float(online[column].iloc[0]) - float(offline[column].iloc[0]))
                row_diffs[column] = diff
                max_numeric_diff = max(max_numeric_diff, diff)
        per_row.append(
            {
                "stationId": str(row.station_id),
                "referenceTime": row.reference_time,
                "maxColumnDiff": max(row_diffs.values()),
            }
        )
        checked += 1
        if checked == 5:
            break
    return {
        "windowsChecked": checked,
        "maxNumericColumnDiffKw": max_numeric_diff,
        "categoricalMismatches": categorical_mismatches,
        "passed": checked == 5 and max_numeric_diff <= 1e-12 and categorical_mismatches == 0,
        "rows": per_row,
    }


def build_bundle(
    manifest: dict,
    subset: list[str],
    models: dict,
    category_levels_full: dict,
    calendar: dict,
    result: dict,
    dropped_features: list[str],
) -> dict:
    category_levels = {
        column: levels
        for column, levels in category_levels_full.items()
        if column in subset
    }
    bounds = common.split_end_exclusive_utc(manifest)
    schema_meta = {
        "schemaVersion": "1.0.0",
        "featureVersion": CANDIDATE_FEATURE_VERSION,
        "modelId": CANDIDATE_MODEL_ID,
        "modelVersion": CANDIDATE_MODEL_VERSION,
        "target": "load",
        "datasetId": manifest["datasetId"],
        "sourceManifestSha256": manifest["sourceManifestSha256"],
        "trainingPublishedBatchId": manifest["publishedBatchId"],
        **bounds,
        "historyHours": int(manifest["mlHistoryHours"]),
        "supportedHorizons": list(CONTRACT_HORIZONS),
        "featureColumns": list(subset),
        "droppedFeatures": list(dropped_features),
        "artifactFile": BUNDLE_PATH.name,
        "artifactSha256": "0" * 64,
        "metrics": {
            "mae": round(result["meanValMaeContract"], 3),
            "rmse": round(result["pooledRmseContract"], 3),
            "testSamples": int(result["contractRowsContract"]),
            "unit": "kW",
        },
        "metricsConvention": (
            "h01/h06/h24 VALIDATION mean MAE, each horizon scored under its own "
            "contract split column (split_1h/split_6h/split_24h), clipped to "
            "[0, rated_capacity_kw]"
        ),
        "validationMeanMaeSplit24hSlice": round(result["meanValMaeSplit24h"], 4),
        "dependencies": {
            "python": platform.python_version(),
            "scikit-learn": sklearn.__version__,
            "pandas": pd.__version__,
            "numpy": np.__version__,
            "joblib": joblib.__version__,
        },
    }
    # NOTE: featureVersion "history24-lean-v1" is a deliberate candidate-side
    # deviation, so model_metadata.schema.json (which pins featureVersion to
    # the const "history24-v1") does NOT validate this block; promotion to a
    # shipped artifact requires a schema/featureVersion release decision.
    payload = {
        "models": models,
        "feature_columns": list(subset),
        "category_levels": category_levels,
        "calendar": calendar,
        "model_id": CANDIDATE_MODEL_ID,
        "model_version": CANDIDATE_MODEL_VERSION,
    }
    pre_bundle = {**payload, "metadata": schema_meta}
    canonical = pickle.loads(pickle.dumps(pre_bundle, protocol=5))
    clean_payload = {key: value for key, value in canonical.items() if key != "metadata"}
    schema_meta["artifactSha256"] = common.canonical_payload_digest(clean_payload)
    return {**clean_payload, "metadata": schema_meta}


def main() -> int:
    started = time.time()
    np.random.seed(SEED)
    manifest = common.read_manifest()
    frame = pd.read_pickle(OUT_DIR / "joined_usable.pkl")
    full_matrix = common.features_matrix(frame)
    calendar = common.build_calendar_lookup(frame)
    category_levels_full = {
        column: sorted(frame[column].astype(str).unique().tolist())
        for column in common.CATEGORICAL_FEATURES
    }

    subsets = candidate_subsets()
    for name, subset in subsets.items():
        print(f"config {name}: {len(subset)} features")

    results: dict[str, dict] = {}
    for name in ("full39", "L1", "L2", "L3"):
        results[name] = evaluate_subset(name, subsets[name], frame, full_matrix)

    baseline_contract = results["full39"]["meanValMaeContract"]
    baseline_split24h = results["full39"]["meanValMaeSplit24h"]
    winner = min(("L1", "L2", "L3"), key=lambda name: results[name]["meanValMaeContract"])
    winner_result = results[winner]
    val_mean = winner_result["meanValMaeContract"]
    # The task gate: improved only if the winning VALIDATION mean beats the
    # shipped hgb-deep baseline 13.2938 by >= 0.05 kW. Note the 13.2938 figure
    # was produced on the split_24h slice (see results json); the contract
    # convention reproduces the full-39 control at `baseline_contract`.
    improved = val_mean <= BASELINE_TO_BEAT - 0.05
    split24h_gate = winner_result["meanValMaeSplit24h"] <= baseline_split24h - 0.05
    winner_agreement = (
        min(("L1", "L2", "L3"), key=lambda name: results[name]["meanValMaeSplit24h"])
    )

    print("\n=== VALIDATION mean MAE of h01/h06/h24 (clipped) ===")
    print(f"{'config':>7}  {'n_feat':>6}  {'contract-mean':>13}  {'split24h-mean':>13}  per-horizon (contract | split24h)")
    for name in ("full39", "L1", "L2", "L3"):
        entry = results[name]
        detail = "  ".join(
            f"h{h:02d} {entry['validation'][f'h{h:02d}']['contract']['mae']:.4f}|"
            f"{entry['validation'][f'h{h:02d}']['split24h']['mae']:.4f}"
            for h in CONTRACT_HORIZONS
        )
        print(
            f"{name:>7}  {entry['nFeatures']:>6}  {entry['meanValMaeContract']:>13.4f}  "
            f"{entry['meanValMaeSplit24h']:>13.4f}  {detail}"
            + ("   <- winner" if name == winner else "")
        )
    print(
        f"contract-convention full-39 control = {baseline_contract:.4f} kW; "
        f"split24h-convention control = {baseline_split24h:.4f} kW (shipped baseline {BASELINE_TO_BEAT}); "
        f"winner {winner} valMean={val_mean:.4f} improved={improved} "
        f"(split24h-convention gate: {split24h_gate}; winner under split24h: {winner_agreement})"
    )

    dropped = [c for c in common.FEATURE_COLUMNS if c not in subsets[winner]]
    bundle = build_bundle(
        manifest,
        subsets[winner],
        winner_result["models"],
        category_levels_full,
        calendar,
        winner_result,
        dropped,
    )
    RACE2_DIR.mkdir(parents=True, exist_ok=True)
    bundle_bytes = pickle.dumps(bundle, protocol=5)
    BUNDLE_PATH.write_bytes(bundle_bytes)
    with open(METADATA_PATH, "w", encoding="utf-8") as handle:
        json.dump(bundle["metadata"], handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    print(f"bundle -> {BUNDLE_PATH} ({len(bundle_bytes) / 1e6:.1f} MB)")

    hourly = common.load_hourly_metrics()
    parity = parity_check(frame, hourly, subsets[winner], calendar)
    print(f"parity check: passed={parity['passed']} windows={parity['windowsChecked']} "
          f"max_numeric_diff={parity['maxNumericColumnDiffKw']:.2e} kW")

    public = {
        "task": "B1 lean feature-pruned candidate sweep (VALIDATION selection only)",
        "selectionConvention": (
            "fit on split_24h==TRAIN with finite label; score h01/h06/h24 on "
            "split_1h/split_6h/split_24h VALIDATION with finite label; clip "
            "predictions to [0, rated_capacity_kw]; rank by mean MAE"
        ),
        "params": PARAMS,
        "seed": SEED,
        "importanceReport": str(IMPORTANCE_REPORT_PATH.relative_to(common.DATA_ANALYSIS_ROOT.parent)),
        "subsets": {name: subsets[name] for name in subsets},
        "results": {
            name: {
                "nFeatures": entry["nFeatures"],
                "meanValMaeContract": entry["meanValMaeContract"],
                "meanValMaeSplit24h": entry["meanValMaeSplit24h"],
                "validation": entry["validation"],
                "contractRowsContract": entry["contractRowsContract"],
                "contractRowsSplit24h": entry["contractRowsSplit24h"],
                "seconds": entry["seconds"],
            }
            for name, entry in results.items()
        },
        "baselineToBeat": BASELINE_TO_BEAT,
        "shippedBaselineConventionNote": (
            "The shipped hgb-deep 13.2938 (12.4897/13.6357/13.7559, n=17425 per "
            "horizon, clipped) was computed on the split_24h VALIDATION slice "
            "for all horizons (train.py convention); the per-horizon contract "
            "split columns give slightly different slices (h01 n=18000, "
            "h06 n=17875, h24 n=17425)"
        ),
        "recomputedFull39BaselineContractMean": baseline_contract,
        "recomputedFull39BaselineSplit24hMean": baseline_split24h,
        "winner": winner,
        "winnerMeanValMaeContract": val_mean,
        "winnerMeanValMaeSplit24h": winner_result["meanValMaeSplit24h"],
        "winnerUnderSplit24hConvention": winner_agreement,
        "improved": improved,
        "improvedGate": "winner contract-convention valMean <= 13.2438",
        "improvedSplit24hConvention": split24h_gate,
        "droppedFeatures": dropped,
        "bundle": {
            "path": str(BUNDLE_PATH),
            "modelId": CANDIDATE_MODEL_ID,
            "modelVersion": CANDIDATE_MODEL_VERSION,
            "featureVersion": CANDIDATE_FEATURE_VERSION,
            "fileSha256": hashlib.sha256(bundle_bytes).hexdigest(),
            "payloadSha256": bundle["metadata"]["artifactSha256"],
            "schemaNote": (
                "metadata block intentionally not valid against "
                "model_metadata.schema.json v1.0.0 (featureVersion const "
                "history24-v1); promotion needs a feature-version release"
            ),
        },
        "parityCheck": parity,
        "environment": {
            "python": platform.python_version(),
            "scikit-learn": sklearn.__version__,
            "pandas": pd.__version__,
            "numpy": np.__version__,
            "joblib": joblib.__version__,
        },
        "runtimeSeconds": round(time.time() - started, 1),
        "generatedAt": datetime.now(timezone.utc).isoformat(),
        "note": "simulated data; VALIDATION only for fitting/selection; TEST never read or scored",
    }
    with open(RESULTS_PATH, "w", encoding="utf-8") as handle:
        json.dump(public, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    print(f"results -> {RESULTS_PATH}  ({time.time() - started:.0f}s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
