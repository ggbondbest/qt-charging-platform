"""Create a compact CSV-only handoff, preserving the logical published batch."""

import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil

from data_analysis.publishing.publish import inspect_export


def bundle_export(input_root, output_root):
    inspected = inspect_export(input_root)
    source = inspected["root"]
    destination = Path(output_root)
    if destination.exists() or destination.is_symlink():
        raise FileExistsError("Bundle output already exists")
    destination = destination.resolve()
    if destination.is_relative_to(source) or source.is_relative_to(destination):
        raise ValueError("Bundle must be independent of its source")
    destination.mkdir(parents=True, exist_ok=False)
    (destination / "_RUNNING").write_text("", encoding="utf-8")
    for relative, path in inspected["files"].items():
        if relative in {"_SUCCESS", "serving_manifest.json"}:
            continue
        output = destination / relative
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, output)
        digest = hashlib.sha256()
        with output.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        if digest.hexdigest() != inspected["fileSha256"][relative]:
            raise ValueError("Bundle copy verification failed; incomplete output retained")
    manifest = copy.deepcopy(inspected["manifest"])
    for table in manifest["tables"].values():
        table.pop("parquetPath", None)
    manifest["storageProfile"] = "PORTABLE_CSV_GZIP"
    with (destination / "serving_manifest.json").open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(manifest, stream, ensure_ascii=False, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    # Validate the copied manifest/schema/checksums BEFORE making the directory
    # consumable. Failed copies deliberately retain their _RUNNING marker.
    inspect_export(destination, require_complete=False)
    latest = inspect_export(source)
    if latest["fileSha256"] != inspected["fileSha256"]:
        raise ValueError("Source batch changed while bundling; incomplete output retained")
    (destination / "_SUCCESS").touch(exist_ok=False)
    (destination / "_RUNNING").unlink()
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args(argv)
    manifest = bundle_export(args.input, args.output)
    print(json.dumps({"datasetId": manifest["datasetId"], "publishedBatchId": manifest["publishedBatchId"],
                      "storageProfile": manifest["storageProfile"]}))


if __name__ == "__main__":
    main()
