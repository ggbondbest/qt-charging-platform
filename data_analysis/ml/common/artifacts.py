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
import re
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
    if not isinstance(metadata, dict):
        raise MetadataError("model metadata must be an object")
    schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    allowed = set(schema["properties"])
    required = set(schema["required"])
    missing = sorted(required - set(metadata))
    if missing:
        raise MetadataError(f"model metadata is missing required keys: {missing}")
    unknown = sorted(set(metadata) - allowed)
    if unknown:
        raise MetadataError(f"model metadata carries unknown keys {unknown}")
    # These safety checks are mandatory even in the lightweight environment without jsonschema.
    for key in ("artifactSha256", "sourceManifestSha256"):
        if not isinstance(metadata[key], str) or not re.fullmatch(r"[0-9a-f]{64}", metadata[key]):
            raise MetadataError(f"invalid {key}")
    if not isinstance(metadata["artifactFile"], str) or not re.fullmatch(
            r"[A-Za-z0-9][A-Za-z0-9_.-]*", metadata["artifactFile"]):
        raise MetadataError("artifactFile must be a local basename")
    if metadata["schemaVersion"] != CONTRACT_VERSION or metadata["featureVersion"] != "history24-v1":
        raise MetadataError("model schema/feature version mismatch")
    if metadata["historyHours"] != 24 or metadata["target"] not in ("availability", "load"):
        raise MetadataError("model target/history contract mismatch")
    horizons = metadata["supportedHorizons"]
    if (not isinstance(horizons, list) or not horizons or
            any(type(value) is not int or value not in (1, 6, 24) for value in horizons) or
            len(set(horizons)) != len(horizons)):
        raise MetadataError("invalid supportedHorizons")
    columns = metadata["featureColumns"]
    if (not isinstance(columns, list) or not columns or
            any(not isinstance(value, str) or not re.fullmatch(r"(?!label_|split_)[a-z][a-z0-9_]*", value)
                for value in columns) or len(set(columns)) != len(columns)):
        raise MetadataError("invalid model feature columns")
    if not isinstance(metadata["dependencies"], dict):
        raise MetadataError("model dependencies must be recorded")
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
    _write_json(directory / REPORT_NAME, {**report, "artifactSha256": metadata["artifactSha256"]})
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
    """Load only a trusted local artifact, after checking its sidecar and exact bytes.

    A SHA256 is an integrity check, not a signature: never accept uploaded pickle files.
    """
    directory = Path(directory).resolve()
    metadata = json.loads((directory / METADATA_NAME).read_text(encoding="utf-8"))
    validate_metadata(metadata)
    artifact = (directory / metadata["artifactFile"]).resolve()
    if artifact.parent != directory:
        raise MetadataError("artifact escapes its bundle directory")
    if sha256_file(artifact) != metadata.get("artifactSha256"):
        raise MetadataError(f"{artifact} does not match the hash recorded in {METADATA_NAME}")
    import sklearn
    dependencies = metadata["dependencies"]
    if dependencies.get("scikit-learn") != sklearn.__version__:
        raise MetadataError("scikit-learn version mismatch; retrain with the installed version")
    if dependencies.get("python", "").split(".")[:2] != [str(sys.version_info.major), str(sys.version_info.minor)]:
        raise MetadataError("Python major/minor version mismatch; retrain locally")
    # Deserialise the same verified file handle, not another filename from the directory.
    with artifact.open("rb") as handle:
        content = handle.read()
        if hashlib.sha256(content).hexdigest() != metadata["artifactSha256"]:
            raise MetadataError("artifact changed while opening it")
        from io import BytesIO
        bundle = joblib.load(BytesIO(content))
    if not isinstance(bundle, dict):
        raise MetadataError("model bundle must be an object")
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
