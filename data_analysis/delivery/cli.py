"""One entry point for the integrated Vue / FastAPI / CPU-ML delivery."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def run(module, *args):
    subprocess.run([sys.executable, "-m", module, *map(str, args)], check=True)


def train():
    # Reuse locally trained, validated artifacts. Published runs are evidence;
    # interrupted or incompatible runs require a separate output, not overwrite.
    run("data_analysis.chargepilot.cli", "train")
    from data_analysis.chargepilot.availability_adapter import AvailabilityAdapter
    output = Path(os.getenv("AVAILABILITY_MODEL_DIR", ROOT / "outputs/ml_availability_delivery"))
    if not output.exists():
        base = output.with_name(output.name + "_base")
        if not (base / "train_summary.json").exists():
            if base.exists() and any(base.iterdir()):
                raise RuntimeError(f"Interrupted run at {base}. Resume/inspect it explicitly or use a new output directory.")
            run("data_analysis.ml.availability.train", "--output", base, "--horizons", 1, 6, 24)
        run("data_analysis.ml.availability.build_hierarchical", "--source-run", base, "--output", output)
    adapter = AvailabilityAdapter(output)
    if adapter.report["status"] != "READY":
        raise RuntimeError(f"Incomplete availability artifacts at {output}; inspect or choose a new output directory")
    run("data_analysis.ml.insights", "train", "--output-dir",
        os.getenv("INSIGHTS_MODEL_DIR", str(ROOT / "outputs/ml_insights_delivery")))
    print("All four model families trained and verified. Next: publish MySQL analytics, configure operational DB, build Vue.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("train", help="train/validate arrival, PR66 load, availability and management insights")
    sub.add_parser("check", help="check both MySQL schemas, exact batch and all models")
    serve = sub.add_parser("serve", help="serve analytics, intelligence, ChargePilot and built Vue on one port")
    serve.add_argument("--host", default="127.0.0.1")
    serve.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()
    for name in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
        os.environ.setdefault(name, "2")
    if args.command == "train":
        train()
        return
    if args.command == "serve":
        import uvicorn
        from threadpoolctl import threadpool_limits
        with threadpool_limits(limits=2):
            uvicorn.run("data_analysis.delivery.app:create_app", factory=True, host=args.host, port=args.port)
        return
    from .app import create_app
    from data_analysis.backend.database import open_snapshot
    app = create_app()
    with open_snapshot(None, mysql_settings=app.state.mysql_settings) as snapshot:
        registry = app.state.models.registry(snapshot.metadata)
    missing = [name for name in ("load", "availability", "insights", "arrival")
               if registry[name]["status"] != "READY"]
    print(json.dumps({"status": "READY" if not missing else "INCOMPLETE", "missingModels": missing,
                      "datasetId": registry["datasetId"], "publishedBatchId": registry["publishedBatchId"],
                      "analytics": "MySQL", "operations": "isolated MySQL",
                      "clock": app.state.operational_app.state.store.clock()}, ensure_ascii=False, indent=2))
    if missing:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
