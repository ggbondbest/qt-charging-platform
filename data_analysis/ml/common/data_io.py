"""Manifest-driven access to the analytics export used for training.

Every table is read from the shard list inside ``serving_manifest.json`` (never from a
guessed glob), each shard is verified against its recorded sha256 and byte size, and the
decoded frame is cached as Parquet under ``outputs/ml_cache`` so repeated training runs do
not decompress 100k+ gzip rows again.  A cache hit is only trusted when the manifest
identity and the per-shard hashes still match.
"""

from __future__ import annotations

import hashlib
import json
import os
import tempfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

import pandas as pd

DATA_ANALYSIS = Path(__file__).resolve().parents[2]
DEFAULT_EXPORT = DATA_ANALYSIS / "datasets" / "analytics_full_180d_v1"
CACHE_ROOT = DATA_ANALYSIS / "outputs" / "ml_cache"

FEATURES_TABLE = "ml_features_hourly"
TARGETS_TABLE = "ml_targets_hourly"
HOURLY_TABLE = "station_hourly_metrics"
SNAPSHOT_TABLE = "station_snapshot"

TIMESTAMP_COLUMNS = {
    "recorded_at": "recorded_at",
    "reference_time": "reference_time",
    "history_start_at": "history_start_at",
    "history_end_at": "history_end_at",
}
DATE_COLUMNS = ("business_date",)


class ExportError(RuntimeError):
    """Raised when the export bundle is missing, incomplete or does not match its manifest."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


@dataclass(frozen=True)
class Export:
    """One verified analytics export bundle plus the identifiers the contracts bind to."""

    root: Path
    manifest: dict
    source_manifest_sha256: str

    @property
    def dataset_id(self) -> str:
        return self.manifest["datasetId"]

    @property
    def published_batch_id(self) -> str:
        return self.manifest["publishedBatchId"]

    @property
    def pipeline_run_id(self) -> str:
        return self.manifest["pipelineRunId"]

    @property
    def feature_version(self) -> str:
        return self.manifest["featureVersion"]

    @property
    def ml_splits(self) -> dict:
        return self.manifest["mlSplits"]

    @property
    def generated_at(self) -> str:
        return self.manifest["generatedAt"]

    @property
    def manifest_sha256(self) -> str:
        """Hash of the export manifest itself, kept apart from the raw-data hash."""
        return _sha256(self.root / "serving_manifest.json")

    def split_column(self, horizon_hours: int) -> str:
        if horizon_hours not in (1, 6, 24):
            raise ExportError("horizon_hours must be 1, 6 or 24")
        return f"split_{horizon_hours}h"

    def table(self, name: str, *, use_cache: bool = True) -> pd.DataFrame:
        """Return ``name`` as a frame with every shard concatenated and keys typed."""
        spec = self.manifest["tables"].get(name)
        if spec is None:
            raise ExportError(f"table {name!r} is not part of {self.published_batch_id}")
        # Verify the actual source even when a local cache exists. A manifest-named cache
        # alone does not prove that its source shards still match that manifest.
        self._verified_paths(spec)
        stamp = hashlib.sha256(f"{self.root}:{name}:{self.manifest_sha256}".encode()).hexdigest()
        cache = CACHE_ROOT / f"{stamp}.parquet"
        sidecar = cache.with_suffix(".json")
        trusted_cache = False
        if use_cache and cache.exists() and sidecar.exists():
            try:
                identity = json.loads(sidecar.read_text(encoding="utf-8"))
                trusted_cache = identity == {"manifestSha256": self.manifest_sha256,
                                             "cacheSha256": _sha256(cache)}
            except (OSError, ValueError):
                trusted_cache = False
        if trusted_cache:
            try:
                frame = pd.read_parquet(cache)
            except Exception:
                trusted_cache = False  # damaged cache is rebuildable, not an alternate data source
        if not trusted_cache:
            frame = self._read_shards(name, spec)
            if use_cache:
                CACHE_ROOT.mkdir(parents=True, exist_ok=True)
                with tempfile.NamedTemporaryFile(dir=CACHE_ROOT, suffix=".parquet", delete=False) as temporary:
                    temporary_path = Path(temporary.name)
                try:
                    frame.to_parquet(temporary_path)
                    identity = {"manifestSha256": self.manifest_sha256, "cacheSha256": _sha256(temporary_path)}
                    os.replace(temporary_path, cache)
                    sidecar.write_text(json.dumps(identity), encoding="utf-8")
                finally:
                    temporary_path.unlink(missing_ok=True)
        if len(frame) != spec["rows"]:
            raise ExportError(f"{name}: manifest declares {spec['rows']} rows, read {len(frame)}")
        expected_columns = [column["name"] for column in spec["columns"]]
        if list(frame.columns) != expected_columns:
            raise ExportError(f"{name}: columns differ from the manifest")
        return frame

    def _verified_paths(self, spec: dict) -> list[Path]:
        paths = []
        for entry in spec["files"]:
            path = (self.root / entry["path"]).resolve()
            if not path.is_relative_to(self.root.resolve()):
                raise ExportError("shard path escapes the export directory")
            if not path.exists():
                raise ExportError(f"missing shard {entry['path']}")
            if path.stat().st_size != entry["bytes"]:
                raise ExportError(f"{entry['path']}: size does not match the manifest")
            if _sha256(path) != entry["sha256"]:
                raise ExportError(f"{entry['path']}: sha256 does not match the manifest")
            paths.append(path)
        return paths

    def _read_shards(self, name: str, spec: dict) -> pd.DataFrame:
        frames = [pd.read_csv(path) for path in self._verified_paths(spec)]
        if not frames:
            raise ExportError(f"{name}: manifest lists no shards")
        frame = pd.concat(frames, ignore_index=True)

        columns = {column["name"]: column for column in spec["columns"]}
        for column in TIMESTAMP_COLUMNS:
            if column in frame.columns:
                frame[column] = pd.to_datetime(frame[column], utc=True, format="ISO8601")
        for column in DATE_COLUMNS:
            if column in frame.columns:
                frame[column] = frame[column].astype("string")
        if columns.get("feature_version") is not None and "feature_version" in frame.columns:
            observed = set(frame["feature_version"].dropna().unique())
            if observed and observed != {self.feature_version}:
                raise ExportError(f"{name}: unexpected feature_version values {sorted(observed)}")
        return frame

    def calendar_flags(self) -> dict[tuple[str, str], tuple[bool, bool]]:
        """(city_id, business_date) -> (is_public_holiday, is_adjusted_workday) as of the batch."""
        frame = self.table(FEATURES_TABLE)
        lookup = frame[["city_id", "business_date", "is_public_holiday", "is_adjusted_workday"]]
        lookup = lookup.drop_duplicates().dropna(subset=["city_id", "business_date"])
        return {
            (city, str(date)): (bool(holiday), bool(workday))
            for city, date, holiday, workday in lookup.itertuples(index=False)
        }


def _short(digest: str) -> str:
    return digest[:12]


def open_export(export_dir: Path | str = DEFAULT_EXPORT) -> Export:
    """Load and sanity-check ``serving_manifest.json`` for one export bundle."""
    root = Path(export_dir).resolve()
    manifest_path = root / "serving_manifest.json"
    if not manifest_path.exists():
        raise ExportError(f"{root} has no serving_manifest.json")
    if not (root / "_SUCCESS").exists():
        raise ExportError(f"{root} has no _SUCCESS marker; the export is incomplete")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest["mlSplits"].get("usable") is not True:
        raise ExportError(f"{root}: mlSplits.usable is false, this batch cannot train a model")
    source_root = DATA_ANALYSIS / "datasets" / manifest["datasetId"]
    if source_root.resolve().parent != (DATA_ANALYSIS / "datasets").resolve():
        raise ExportError("datasetId must name a local dataset directory")
    source_manifest = source_root / "manifest.json"
    if not source_manifest.exists():
        raise ExportError(f"raw manifest {source_manifest} is missing; batches cannot be traced")
    recorded = _sha256(source_manifest)
    if recorded != manifest["sourceManifestSha256"]:
        raise ExportError(
            "raw manifest hash does not match the export manifest: "
            f"{recorded} != {manifest['sourceManifestSha256']}"
        )
    return Export(root=root, manifest=manifest, source_manifest_sha256=recorded)


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")
