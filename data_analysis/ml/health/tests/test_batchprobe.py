"""batchprobe（跨批次泛化探针）的纯函数测试——不依赖任何已生成产物，tmp 目录自给自足。

这些测试守的是探针的**纪律面**：dtype 逐列直通不悄悄改型、列集不一致 fail loud、
manifest 永远显式标注非发布且拒绝就地覆盖、散布统计数值正确。
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pandas as pd
import pytest

from data_analysis.ml.health import batchprobe


def test_clean_column_datetime_strips_z_without_shift():
    raw = pd.Series(["2026-01-02T03:04:05Z", "2026-05-29T13:10:00Z"])
    out = batchprobe.clean_column(raw, "datetime64[ns]", "maintenance_tickets", "reported_at")
    assert str(out.dtype) == "datetime64[ns]"
    assert out.dt.tz is None
    assert out.iloc[0] == pd.Timestamp("2026-01-02 03:04:05")   # 去 Z、不移相（发布约定）


def test_clean_column_int_fail_loud_on_junk():
    with pytest.raises(ValueError):
        batchprobe.clean_column(pd.Series(["1", "abc"]), "int64", "t", "c")


def test_clean_column_float_and_object_nan_conventions():
    floats = batchprobe.clean_column(pd.Series(["1.5", np.nan]), "float64", "t", "c")
    assert floats.dtype == "float64" and np.isnan(floats.iloc[1])
    objects = batchprobe.clean_column(pd.Series(["x", np.nan]), "object", "t", "c")
    assert objects.iloc[0] == "x" and objects.iloc[1] is None    # parquet null 对齐发布形状


def test_clean_column_unknown_dtype_rejected():
    with pytest.raises(AssertionError, match="未处理的 dtype"):
        batchprobe.clean_column(pd.Series([1]), "category", "t", "c")


def _write_raw_table(raw_dir, table: str, frame: pd.DataFrame) -> None:
    dest = Path(raw_dir) / table
    dest.mkdir(parents=True, exist_ok=True)
    frame.to_csv(dest / "part-00000.csv.gz", index=False)


def test_build_clean_passes_through_and_refuses_column_drift(tmp_path):
    ref_dir = tmp_path / "ref"
    stations = pd.DataFrame({"station_id": pd.Series(["S1"], dtype="object"),
                             "transformer_kw": pd.Series([360.0], dtype="float64"),
                             "opened_on": pd.Series(pd.to_datetime(["2024-01-01"]),
                                                    dtype="datetime64[ns]")})
    _write_raw_table(tmp_path / "raw", "stations",
                     pd.DataFrame({"station_id": ["S1", "S2"], "transformer_kw": ["360.0", "360.0"],
                                   "opened_on": ["2024-01-01T00:00:00Z", "2024-02-01T00:00:00Z"]}))
    ref_clean = ref_dir / "clean"
    (ref_clean / "stations").mkdir(parents=True)
    stations.to_parquet(ref_clean / "stations" / "part-00000.snappy.parquet", index=False)
    counts = batchprobe.build_clean(tmp_path / "raw", tmp_path / "clean", ref_clean,
                                    tables=["stations"])
    assert counts == {"stations": 2}
    built = pd.read_parquet(tmp_path / "clean" / "stations" / "part-00000.snappy.parquet")
    assert list(built.columns) == list(stations.columns)          # 列序与基准一致
    assert str(built["transformer_kw"].dtype) == "float64"
    assert built["opened_on"].iloc[0] == pd.Timestamp("2024-01-01")
    # 列集合漂移必须炸
    _write_raw_table(tmp_path / "raw2", "stations",
                     pd.DataFrame({"station_id": ["S1"], "typo_col": ["360.0"],
                                   "opened_on": ["2024-01-01T00:00:00Z"]}))
    with pytest.raises(AssertionError, match="typo_col"):
        batchprobe.build_clean(tmp_path / "raw2", tmp_path / "clean2", ref_clean,
                               tables=["stations"])


def test_ticket_spread_matches_hand_computation():
    tickets = pd.DataFrame({"charger_id": ["A", "B", "B", "B"]})
    spread = batchprobe.ticket_spread(tickets, days=2)
    assert spread["chargers"] == 2 and spread["tickets"] == 4
    assert spread["sdObserved"] == round(np.sqrt(0.5), 5)
    assert spread["sdIidBinomial"] == round(np.sqrt(2) / 2, 5)
    assert spread["overdispersionRatio"] == 1.0                   # 手工挑的等例：恰好 iid


def test_build_summary_delta_and_missing_side(tmp_path):
    def report(chosen_set, model_auc):
        path = tmp_path / f"{chosen_set}_{model_auc}.json"
        path.write_text(json.dumps({"chosenSet": chosen_set, "chosenHyper": "h",
                                    "risk": {"baseRate": 0.19},
                                    "scoreboard": {"model": {"auc": model_auc},
                                                   "naive": {"auc": 0.5}}}), encoding="utf-8")
        return path
    pub, probe = report("staticOnly", 0.6219), report("opsOnly", 0.4366)
    tickets = pd.DataFrame({"charger_id": ["A", "B", "B", "B"]})
    summary = batchprobe.build_summary(probe, pub, tickets, tickets, days=2)
    assert summary["aucComparison"]["model"]["delta"] == pytest.approx(0.4366 - 0.6219, abs=1e-9)
    assert summary["chosen"] == {"published": "staticOnly/h", "probe": "opsOnly/h"}
    # 发布侧缺产物（新克隆）：不崩，对照留空、散布标注不可用
    empty = batchprobe.build_summary(probe, tmp_path / "nonexistent.json", tickets,
                                     pd.DataFrame(columns=["charger_id"]), days=2)
    assert empty["aucComparison"]["model"]["published"] is None
    assert "unavailable" in empty["ticketSpread"]["published"]


def test_probe_manifest_is_explicitly_not_a_publication(tmp_path):
    published = {"mlSplits": {"start": "2025-12-02", "end": "2026-05-29"},
                 "definitions": {"label": "x"}, "publishedBatchId": "analytics-298aa3ee"}
    probe_root = tmp_path / "probe"
    probe_root.mkdir()
    (probe_root / "manifest.json").write_text(
        json.dumps({"dataset_id": "charging_probe_t1", "config": {"dirty_rate": 0.0, "seed": 7}}),
        encoding="utf-8")
    batch_id = batchprobe.write_probe_manifest(probe_root, published, {"stations": 2},
                                               "charging_probe_t1")
    assert batch_id.startswith("analytics-probe-") and batch_id != "analytics-298aa3ee"
    manifest = json.loads((probe_root / "serving_manifest.json").read_text(encoding="utf-8"))
    assert manifest["storageProfile"] == "DEV_PROBE_NOT_PUBLISHED"
    assert manifest["pipelineRunId"] == "dev-probe-pass-through-not-spark"
    assert manifest["mlSplits"] == published["mlSplits"]         # 几何原样复用
    assert "DEV PROBE" in manifest["definitions"]["batch_class"]
    with pytest.raises(FileExistsError):                          # 独占创建：不许二写
        batchprobe.write_probe_manifest(probe_root, published, {"stations": 2}, "charging_probe_t1")
