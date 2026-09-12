"""One command for local raw -> real Spark -> exports -> verified SQLite.

For HDFS use the individual pipeline/export modules with hdfs:// paths, then
download only the exported CSV/metadata for publication. Do not claim local
execution verifies an unavailable Hadoop server.
"""

import argparse
import json
from pathlib import Path


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True, help="New local run directory")
    parser.add_argument("--master", default="local[2]")
    parser.add_argument("--shuffle-partitions", type=int, default=8)
    parser.add_argument("--driver-memory", help="JVM heap before startup, e.g. 2g or 4g; default: submit environment or 2g")
    args = parser.parse_args(argv)
    if "://" in args.input or "://" in args.output:
        parser.error("This convenience entry point uses local paths; use pipeline/export for HDFS")
    source, root = Path(args.input).resolve(), Path(args.output).resolve()
    if Path(args.output).is_symlink() or root.exists():
        parser.error("Output must not already exist")
    if root.is_relative_to(source) or source.is_relative_to(root):
        parser.error("Input/output must not overlap")
    if args.shuffle_partitions < 1:
        parser.error("shuffle-partitions must be positive")
    from data_analysis.spark_jobs.pipeline import create_spark, run_pipeline
    from data_analysis.spark_jobs.export import export_data
    from data_analysis.publishing.publish import publish_dataset
    try:
        spark = create_spark("charging-complete-data-layer", args.master, args.shuffle_partitions, args.driver_memory)
    except ValueError as exc:
        parser.error(str(exc))
    try:
        root.mkdir(parents=True, exist_ok=False)
        (root / "_RUNNING").write_text("", encoding="utf-8")
        run_pipeline(spark, str(source), str(root / "processed"))
        manifest = export_data(spark, str(source), str(root / "processed"), str(root / "export"))
    finally:
        spark.stop()
    report = publish_dataset(root / "export", root / "analytics.sqlite3")
    (root / "_RUNNING").unlink()
    (root / "_SUCCESS").write_text("", encoding="utf-8")
    print(json.dumps({"datasetId": manifest["datasetId"], "publishedBatchId": manifest["publishedBatchId"],
                      "database": str(root / "analytics.sqlite3"), "publication": report}, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
