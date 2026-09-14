"""Assemble the availability training frame and write a data profile.

    python -m data_analysis.ml.availability.prepare_data --output data_analysis/outputs/ml_avail_run1

No Spark, no SQLite: this reads only the two ML tables plus the hourly and snapshot tables the
same export bundle already ships, and verifies every shard against ``serving_manifest.json``.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from data_analysis.ml.common import artifacts, forecaster
from data_analysis.ml.common.data_io import DEFAULT_EXPORT


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--export", default=str(DEFAULT_EXPORT), help="analytics export bundle directory")
    parser.add_argument("--output", required=True, help="run directory to write the profile into")
    arguments = parser.parse_args(argv)

    frame = forecaster.build_frame(Path(arguments.export))
    output = Path(arguments.output)
    output.mkdir(parents=True, exist_ok=True)
    profile = {
        "datasetId": frame.export.dataset_id,
        "pipelineRunId": frame.export.pipeline_run_id,
        "publishedBatchId": frame.export.published_batch_id,
        "sourceManifestSha256": frame.export.source_manifest_sha256,
        "servingManifestSha256": frame.export.manifest_sha256,
        "rows": int(len(frame.data)),
        "stations": len(frame.stations),
        "cities": frame.cities,
        "siteTypes": frame.site_types,
        "capacity": sorted({station["capacity"] for station in frame.stations.values()}),
        "splits": forecaster.split_summary(frame),
        "featureColumns": frame.feature_columns,
        "preparedAt": artifacts.stamp(),
    }
    (output / "data_profile.json").write_text(
        json.dumps(profile, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: profile[key] for key in ("rows", "stations", "splits")}, ensure_ascii=False))
    print(f"profile written to {output / 'data_profile.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
