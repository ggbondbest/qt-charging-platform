"""Model bundle and metadata handling for the ``ml`` tasks.

``model_metadata.schema.json`` is strict: it has ``additionalProperties: false``, so only the
contract fields go into ``model_metadata.json``.  Everything richer (per-step errors, coverage,
baselines, seeds) belongs in ``evaluation_report.json`` next to it.

``sourceManifestSha256`` binds the **raw dataset manifest**, per contracts/README.md; the hash of
the export's own ``serving_manifest.json`` is recorded separately in the evaluation report.
"""

from __future__ import annotations

import hashlib
import json
import platform
import sys
from datetime import datetime, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

import joblib

CONTRACT_VERSION = "1.0.0"
BUSINESS_ZONE = ZoneInfo("Asia/Shanghai")
SCHEMA_PATH = Path(__file__).resolve().parents[2] / "contracts" / "model_metadata.schema.json"
ARTIFACT_NAME = "model.joblib"
METADATA_NAME = "model_metadata.json"
REPORT_NAME = "training_report.json"


class MetadataError(ValueError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def dependency_versions() -> dict[str, str]:
    import numpy
    import pandas
    import sklearn

    return {
        "python": f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}",
        "platform": platform.platform(),
        "numpy": numpy.__version__,
        "pandas": pandas.__version__,
        "scikit-learn": sklearn.__version__,
        "joblib": joblib.__version__,
    }


def business_midnight_utc(date_text: str) -> str:
    """Shanghai midnight of a business date, expressed as the UTC instant it starts."""
    local = datetime.strptime(date_text, "%Y-%m-%d").replace(tzinfo=BUSINESS_ZONE)
    return local.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def build_metadata(
    *,
    model_id: str,
    model_version: str,
    target: str,
    feature_version: str,
    dataset_id: str,
    source_manifest_sha256: str,
    training_published_batch_id: str,
    splits: dict,
    feature_columns: list[str],
    artifact_file: str,
    artifact_sha256: str,
    supported_horizons: list[int],
    metrics: dict,
) -> dict:
    metadata = {
        "schemaVersion": CONTRACT_VERSION,
        "featureVersion": feature_version,
        "modelId": model_id,
        "modelVersion": model_version,
        "target": target,
        "datasetId": dataset_id,
        "sourceManifestSha256": source_manifest_sha256,
        "trainingPublishedBatchId": training_published_batch_id,
        "trainEndExclusive": business_midnight_utc(splits["trainEnd"]),
        "validationEndExclusive": business_midnight_utc(splits["validationEnd"]),
        "testEndExclusive": business_midnight_utc(splits["end"]),
        "historyHours": 24,
        "supportedHorizons": supported_horizons,
        "featureColumns": list(feature_columns),
        "artifactFile": artifact_file,
        "artifactSha256": artifact_sha256,
        "metrics": metrics,
        "dependencies": dependency_versions(),
    }
    validate_metadata(metadata)
    return metadata


def validate_metadata(metadata: dict) -> None:
    schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    allowed = set(schema["properties"])
    required = set(schema["required"])
    missing = sorted(required - set(metadata))
    if missing:
        raise MetadataError(f"model metadata is missing required keys: {missing}")
    unknown = sorted(set(metadata) - allowed)
    if unknown:
        raise MetadataError(f"model metadata carries unknown keys {unknown}")
    try:
        import jsonschema
    except ImportError:
        return
    errors = sorted(jsonschema.Draft202012Validator(schema).iter_errors(metadata), key=lambda e: list(e.path))
    if errors:
        detail = "; ".join(f"{'/'.join(map(str, error.path)) or '<root>'}: {error.message}" for error in errors[:5])
        raise MetadataError(f"model metadata does not satisfy {SCHEMA_PATH.name}: {detail}")


def save_bundle(directory: Path, bundle: dict, metadata: dict, report: dict) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    artifact = directory / ARTIFACT_NAME
    joblib.dump(bundle, artifact)
    metadata = dict(metadata)
    metadata["artifactFile"] = ARTIFACT_NAME
    metadata["artifactSha256"] = sha256_file(artifact)
    validate_metadata(metadata)
    _write_json(directory / METADATA_NAME, metadata)
    _write_json(directory / REPORT_NAME, report)
    return artifact


def bundle_directories(run_dir: Path) -> list[Path]:
    """Every saved bundle inside a run directory, at depth 1 (``run/h06``) or 2 (``run/coldstart_DL/h06``)."""
    run_dir = Path(run_dir)
    found: list[Path] = []
    for entry in sorted(run_dir.iterdir()):
        if not entry.is_dir():
            continue
        if (entry / METADATA_NAME).exists():
            found.append(entry)
            continue
        for nested in sorted(entry.iterdir()):
            if nested.is_dir() and (nested / METADATA_NAME).exists():
                found.append(nested)
    return found


def load_bundle(directory: Path) -> tuple[dict, dict]:
    directory = Path(directory)
    bundle = joblib.load(directory / ARTIFACT_NAME)
    metadata = json.loads((directory / METADATA_NAME).read_text(encoding="utf-8"))
    artifact = directory / metadata.get("artifactFile", ARTIFACT_NAME)
    if sha256_file(artifact) != metadata.get("artifactSha256"):
        raise MetadataError(f"{artifact} does not match the hash recorded in {METADATA_NAME}")
    return bundle, metadata


def _write_json(path: Path, payload: dict) -> None:
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def invocation(module: str, argv: list[str] | None = None) -> str:
    """The command exactly as typed, with the module path restored (``argv[0]`` is a file path).

    Recorded inside every bundle's ``training_report.json``: the handoff contract asks for a
    reproducible training command as a deliverable, and a report that only names the script leaves
    the flags, seed and batch choice to whoever remembers the shell history.

    ``-m`` is what the report has to say, because that is the documented way to run these entries
    from the repository root - but a module executed that way sees ``__name__ == "__main__"``, so
    the dotted path is recovered from the run spec instead of the name argument.  A file launched
    directly has no spec to ask, and there ``-m <path>`` would be a command that does not run.
    """
    arguments = list(sys.argv[1:] if argv is None else argv)
    if module == "__main__":
        spec = getattr(sys.modules.get("__main__"), "__spec__", None)
        name = getattr(spec, "name", None)
        if not name:
            return " ".join(["python", str(sys.argv[0])] + arguments)
        module = name
    return " ".join(["python", "-m", module] + arguments)
