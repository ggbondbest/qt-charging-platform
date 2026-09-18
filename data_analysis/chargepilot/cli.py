"""Cross-platform entry point, always invoked from repository root."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

from .settings import ROOT


def evaluate_load(output_dir):
    """Bind a newly produced PR66 TEST report to the exact evaluated bytes."""
    from data_analysis.ml.load.common import MODEL_ID, read_manifest
    from .experiment import atomic_report
    bundle = output_dir/f"{MODEL_ID}.joblib"
    before = hashlib.sha256(bundle.read_bytes()).hexdigest()
    subprocess.run([sys.executable, "-m", "data_analysis.ml.load.evaluate"], check=True)
    after = hashlib.sha256(bundle.read_bytes()).hexdigest()
    if before != after:
        raise RuntimeError("Load artifact changed during evaluation; result not bound")
    suffix = read_manifest()["publishedBatchId"].removeprefix("analytics-")[:8]
    report = output_dir/f"test_metrics_{MODEL_ID}_{suffix}.json"
    atomic_report(output_dir/"chargepilot_evaluation_binding.json",
                  {"artifactSha256": before, "reportSha256": hashlib.sha256(report.read_bytes()).hexdigest(),
                   "reportFile": report.name})


def main():
    parser = argparse.ArgumentParser(description="ChargePilot CPU ML + Vue + MySQL demo")
    sub = parser.add_subparsers(dest="command", required=True)
    train = sub.add_parser("train", help="prepare/train missing local trusted artifacts")
    train.add_argument("--skip-load", action="store_true", help="only train arrival/wait; load endpoint remains503")
    sub.add_parser("evaluate-load", help="new frozen TEST evaluation + exact artifact binding (refuses existing report)")
    serve = sub.add_parser("serve", help="start API and built Vue frontend")
    serve.add_argument("--host", default="127.0.0.1")
    serve.add_argument("--port", type=int, default=8000)
    exp = sub.add_parser("experiment", help="calculate paired replay, no database mutations")
    exp.add_argument("--users", type=int, default=1000)
    exp.add_argument("--seed", type=int, default=42)
    export = sub.add_parser("export-feedback", help="export operational feedback for future offline training")
    export.add_argument("--output", type=Path, default=ROOT/"outputs"/"chargepilot"/"feedback.json")
    sub.add_parser("check", help="validate model files and MySQL connectivity")
    args = parser.parse_args()
    # Avoid a small request spawning all CPU cores; no GPU/Hadoop needed for inference.
    os.environ.setdefault("OMP_NUM_THREADS", "2")
    os.environ.setdefault("OPENBLAS_NUM_THREADS", "2")
    os.environ.setdefault("MKL_NUM_THREADS", "2")
    model_dir = Path(os.getenv("CHARGEPILOT_MODEL_DIR", ROOT/"outputs"/"chargepilot"))
    load_dir = Path(os.getenv("CHARGEPILOT_LOAD_DIR", ROOT/"outputs"/"ml_load"))
    if args.command == "train":
        if not (model_dir/"arrival.metadata.json").exists():
            subprocess.run([sys.executable, "-m", "data_analysis.chargepilot.ml.train", "--output", str(model_dir)], check=True)
        else:
            from .ml.predict import ArrivalPredictor
            ArrivalPredictor(model_dir)
            print("Validated existing arrival artifact; not overwriting trained model")
        if not args.skip_load:
            from data_analysis.ml.load.common import MODEL_ID
            if not (load_dir/f"{MODEL_ID}.joblib").exists():
                if load_dir.resolve() != (ROOT/"outputs"/"ml_load").resolve():
                    raise ValueError("PR66 training writes outputs/ml_load. Train there first, then copy trusted complete artifacts to CHARGEPILOT_LOAD_DIR")
                for module in ("prepare_data", "train"):
                    subprocess.run([sys.executable, "-m", f"data_analysis.ml.load.{module}"], check=True)
                evaluate_load(load_dir)
            from .load_adapter import LoadAdapter
            adapter = LoadAdapter(load_dir)
            print("PR66 load model:", adapter.report["status"])
        print("Models ready. Next: configure isolated MySQL, npm ci/build, then cli serve")
    elif args.command == "evaluate-load":
        if load_dir.resolve() != (ROOT/"outputs"/"ml_load").resolve():
            raise ValueError("PR66 evaluation must run on outputs/ml_load before transferring artifacts")
        evaluate_load(load_dir)
    elif args.command == "experiment":
        from .ml.predict import ArrivalPredictor
        from .experiment import run_experiment
        result = run_experiment(ArrivalPredictor(model_dir), args.users, args.seed, model_dir/"experiment.json")
        print(json.dumps(result, ensure_ascii=False, indent=2))
    elif args.command == "serve":
        import uvicorn
        from threadpoolctl import threadpool_limits
        with threadpool_limits(limits=2):
            uvicorn.run("data_analysis.chargepilot.app:create_app", factory=True, host=args.host, port=args.port)
    elif args.command == "check":
        from .app import create_app
        app = create_app()
        print(json.dumps({"model": app.state.predictor.metadata["modelId"],
                          "database": "MySQL", "clock": app.state.store.clock()}, ensure_ascii=False, indent=2))
    elif args.command == "export-feedback":
        from .settings import Settings
        from .store import ChargePilotStore
        from .experiment import atomic_report
        store = ChargePilotStore(Settings.from_env().mysql)
        records = store.feedback()
        atomic_report(args.output, {"dataSource": "SIMULATED_OPERATIONAL_FEEDBACK", "records": records,
                                   "note": "Not automatically used to retrain the evaluated model"})
        print(f"Exported {len(records)} records to {args.output}")


if __name__ == "__main__":
    main()
