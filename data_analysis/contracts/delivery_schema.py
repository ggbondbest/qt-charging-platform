"""Export/check the unified API without a DB, model artifacts or credentials.

Base analytics contracts remain independently consumable. This additional
schema captures delivery-only advisor/forecast routes at the same origin.
Run ``python -m data_analysis.contracts.delivery_schema [--check]``.
"""
import argparse
import json
from pathlib import Path
from types import SimpleNamespace

from .generate_types import generate

ROOT = Path(__file__).resolve().parent


def schema():
    from fastapi import FastAPI
    from data_analysis.chargepilot.settings import Settings
    from data_analysis.delivery.app import create_app

    # Schema-only provider: no methods can perform inference or open a file.
    app = create_app(settings=Settings(), database_path=":memory:",
                     provider=SimpleNamespace(arrival=None, load=None),
                     operational_app=FastAPI())
    try:
        return app.openapi()
    finally:
        app.state.advisor_runner.close()


def artifacts():
    encoded = json.dumps(schema(), ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    return {"delivery_openapi.json": encoded,
            "delivery_types.ts": generate(json.loads(encoded), "contracts/delivery_openapi.json")}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    for name, content in artifacts().items():
        path = ROOT / name
        if args.check:
            if not path.is_file() or path.read_text(encoding="utf8") != content:
                parser.exit(1, f"Unified delivery contract is missing or stale: {name}\n")
        else:
            path.write_text(content, encoding="utf8", newline="\n")


if __name__ == "__main__":
    main()
